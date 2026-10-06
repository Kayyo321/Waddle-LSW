/** @file icd.c @brief Native experimental ICD dispatch/lifecycle/loss fixture.
 */
#include "vn_cs.h"
#include "waddle/venus_icd.h"
#include "shaders/compute_shader.h"
#include "shaders/triangle_vertex_shader.h"
#include "shaders/triangle_fragment_shader.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#include "vn_protocol_driver_buffer.h"
#include "vn_protocol_driver_image.h"
#include "vn_protocol_driver_shader_module.h"
#include "vn_protocol_driver_descriptor_set_layout.h"
#include "vn_protocol_driver_descriptor_pool.h"
#include "vn_protocol_driver_descriptor_set.h"
#include "vn_protocol_driver_pipeline_layout.h"
#include "vn_protocol_driver_pipeline.h"
#include "vn_protocol_driver_image_view.h"
#include "vn_protocol_driver_render_pass.h"
#include "vn_protocol_driver_framebuffer.h"
#include "vn_protocol_driver_device.h"
#include "vn_protocol_driver_command_buffer.h"
#include "vn_protocol_driver_command_pool.h"
#include "vn_protocol_driver_device_memory.h"
#include "vn_protocol_driver_fence.h"
#include "vn_protocol_driver_semaphore.h"
#include "vn_protocol_driver_queue.h"
#pragma GCC diagnostic pop
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef VgpuIcdLoader
#include <stdlib.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#endif
#if defined(VgpuMappingAllocationFaults) && !defined(_WIN32)
#include <errno.h>
/** @brief One-shot native shadow allocation failure injection, sole fixture thread. */
static _Atomic unsigned mapping_fail_allocation;
/** @brief Linker ABI for libc allocation, without ownership changes.
 * @param[out] output Nonnull borrowed pointer storage, allocated pointer on success.
 * @param[in] alignment Valid power-of-two alignment. @param[in] size Allocation bytes.
 * @return Zero transfers allocation to caller for libc free; errno code on failure.
 * @note Native libc thread-safe allocation; test-only linker wrapper target.
 */
extern int __real_posix_memalign(void **output, size_t alignment, size_t size);
/** @brief Fail exactly one aligned ICD shadow allocation before any export.
 * @param[out] output Nonnull borrowed pointer storage, unchanged on injected ENOMEM.
 * @param[in] alignment Native allocation alignment. @param[in] size Native bytes.
 * @return ENOMEM when armed for4096-aligned16384-byte shadow, otherwise libc result.
 * @note Test-only atomic injection; successful allocation owner still caller/free.
 */
int __wrap_posix_memalign(void **output, size_t alignment, size_t size) {
    if (alignment == 4096 && size == 16384 &&
        atomic_exchange_explicit(&mapping_fail_allocation, 0, memory_order_relaxed))
        return ENOMEM;
    return __real_posix_memalign(output, alignment, size);
}
#endif
/** @brief Test-only immutable renderer encoder oracle, borrows output for the
 * call.
 * @param[in] kind Pinned query3/6/8.
 * @param[out] bytes Nonnull output[capacity], exclusive to test.
 * @param[in] capacity Actual accessible extent, at least4096.
 * @return Encoded byte extent, zero invalid local input.
 * @note Allocation-free, synchronous; defined in independent values fixture.
 */
size_t venus_values_test_encode(uint32_t kind, void *bytes, size_t capacity);
/** @brief Independent renderer oracle for exact core properties.
 * @param[in] properties Nonnull borrowed native value, no retention.
 * @param[out] bytes Nonnull exclusive accessible bytes[capacity].
 * @param[in] capacity At least4096. @return Encoded prefix extent, zero invalid arguments.
 * @note Allocation-free, synchronous; disjoint outputs thread-safe.
 */
size_t venus_values_test_properties(const VkPhysicalDeviceProperties *properties, void *bytes, size_t capacity);
/** @brief External Vulkan loader ABI alias, no storage or ownership. */
extern VkResult
negotiate_external(uint32_t *version) __asm__("vk_icdNegotiateLoaderICDInterfaceVersion");
/** @brief External Vulkan loader ABI alias; pointers are static/borrowed. */
extern PFN_vkVoidFunction lookup_external(VkInstance instance,
                                          const char *name) __asm__("vk_icdGetInstanceProcAddr");
/** @brief Test backend owns only its private reply bytes; no native Vulkan
 * implementation. */
typedef struct fixture_t {
    unsigned char reply[4096];
    uint32_t submissions;
    uint32_t enumerations;
    uint32_t command;
    uint32_t fail_command;
    uint32_t corrupt_command;
    uint32_t poll_again;
    uint32_t reply_again;
    int32_t create_result;
    int32_t enumerate_result;
    int32_t transport_failure;
    uint32_t device_count;
    unsigned fail_fill;
    const VkDeviceCreateInfo *device_info;
    const VkBufferCreateInfo *buffer_info;
    const VkRenderPassCreateInfo *render_pass_info;
    const VkFramebufferCreateInfo *framebuffer_info;
    const VkImageCreateInfo *image_info;
    const VkImageViewCreateInfo *view_info;
    const VkShaderModuleCreateInfo *shader_info;
    const VkComputePipelineCreateInfo *compute_info;
    const VkGraphicsPipelineCreateInfo *graphics_info;
    const VkDescriptorSetLayoutCreateInfo *descriptor_layout_info;
    const VkPipelineLayoutCreateInfo *pipeline_layout_info;
    const VkDescriptorPoolCreateInfo *descriptor_pool_info;
    const VkDescriptorSetAllocateInfo *descriptor_allocate_info;
    const VkWriteDescriptorSet *descriptor_writes;
    const VkCopyDescriptorSet *descriptor_copies;
    uint32_t descriptor_write_count;
    uint32_t descriptor_copy_count;
    unsigned descriptor_properties;
    unsigned push_limit_mode;
    unsigned requirements_fault;
    uint64_t requirements_size;
    const void *update_data;
    const VkMemoryAllocateInfo *memory_info;
    int32_t bind_result;
    const VkCommandPoolCreateInfo *pool_info;
    int32_t pool_reset_result;
    const VkCommandBufferAllocateInfo *command_allocate;
    const VkCommandBufferBeginInfo *command_begin;
    int32_t command_result;
    unsigned command_array_fault;
    unsigned semaphore_fault;
    int32_t submit_result;
    uint64_t submit_fence;
    uint64_t gpu_issued[64];
    uint32_t gpu_pending;
    _Atomic unsigned idle_issued;
    unsigned retire_on_submit;
    uint32_t gpu_issue_pending;
    uint32_t gpu_corrupt;
    int32_t gpu_failure;
    unsigned char fence_ready[4096];
    uint32_t fence_pending;
    unsigned properties_override;
    uint32_t properties_version;
    unsigned mapping_enabled;
    unsigned mapping_noncoherent;
    uint32_t mapping_fail_kind;
    int32_t mapping_failure;
    unsigned mapping_corrupt;
    uint32_t mapping_creates;
    uint32_t mapping_frees;
    uint32_t mapping_reads;
    uint32_t mapping_writes;
    unsigned char mapping_live[64];
    unsigned char mapping_storage[32768];
    unsigned fence_override;
    int32_t fence_result;
} fixture_t;
static uint32_t read_u32(const void *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}
static uint64_t read_u64(const void *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}
static void put_u32(void *bytes, uint32_t value) { memcpy(bytes, &value, sizeof(value)); }
static void put_u64(void *bytes, uint64_t value) { memcpy(bytes, &value, sizeof(value)); }
static venus_ring_status_t exchange(void *context, const venus_request_t *request,
                                    const void *input, size_t length, venus_request_t *response,
                                    void *output, size_t capacity) {
    fixture_t *fixture = context;
    *response = (venus_request_t){.kind = request->kind, .direction = 1};
    if (request->kind >= RequestCreate && request->kind <= RequestWrite) {
        assert(fixture->mapping_enabled && request->resource_id >= 2 && request->resource_id <= 65);
        if (request->kind == fixture->mapping_fail_kind) return fixture->mapping_failure;
        unsigned slot = request->resource_id - 2;
        if (request->kind == RequestCreate) {
            assert(!input && !length && !output && !capacity && request->flags == 1 &&
                request->argument_zero && request->argument_one && request->argument_one % 4096 == 0);
            assert(!fixture->mapping_live[slot]);
            fixture->mapping_live[slot] = 1; fixture->mapping_creates++;
        } else if (request->kind == RequestFree) {
            assert(fixture->mapping_live[slot] && !input && !output);
            fixture->mapping_live[slot] = 0; fixture->mapping_frees++;
        } else {
            assert(fixture->mapping_live[slot] && request->argument_one <= 4096 &&
                request->argument_zero <= sizeof(fixture->mapping_storage) - request->argument_one);
            if (request->kind == RequestRead) {
                assert(output && capacity == request->argument_one && !input && !length);
                memcpy(output, fixture->mapping_storage + request->argument_zero, capacity);
                response->payload_bytes = (uint32_t)capacity; fixture->mapping_reads++;
            } else {
                assert(input && length == request->argument_one && !output && !capacity);
                memcpy(fixture->mapping_storage + request->argument_zero, input, length);
                fixture->mapping_writes++;
            }
        }
        if (fixture->mapping_corrupt == 1) response->flags = 1;
        if (fixture->mapping_corrupt == 2 && request->kind == RequestRead) response->payload_bytes = 0;
        return RingOk;
    }
    if (request->kind == RequestGpuFence || request->kind == RequestGpuPoll) {
        assert(!input && !length && !output && !capacity && request->argument_zero > 0 &&
               request->argument_zero < 64);
        if (fixture->gpu_failure)
            return fixture->gpu_failure;
        if (request->kind == RequestGpuFence) {
            assert(!request->argument_one);
            if (fixture->gpu_issue_pending) {
                fixture->gpu_issue_pending--;
                return RingAgain;
            }
            response->argument_zero = ++fixture->gpu_issued[request->argument_zero];
            atomic_store_explicit(&fixture->idle_issued, 1, memory_order_release);
            if (fixture->gpu_corrupt == 1)
                response->argument_zero = 0;
        } else {
            assert(request->argument_one == fixture->gpu_issued[request->argument_zero]);
            if (fixture->gpu_pending) {
                fixture->gpu_pending--;
                return RingAgain;
            }
        }
        if (fixture->gpu_corrupt == 2)
            response->flags = 1;
        if (fixture->gpu_corrupt == 3 && request->kind == RequestGpuPoll)
            response->argument_zero = 1;
        return RingOk;
    }
    if (request->kind == RequestSubmit) {
        assert(input && length >= 44 && !output && !capacity);
        const unsigned char *bytes = (const unsigned char *)input + 36;
        fixture->command = read_u32(bytes);
        if (fixture->command == fixture->fail_command ||
            (fixture->command == 2 && fixture->fail_fill && read_u32(bytes + 24)))
            return fixture->transport_failure;
        fixture->submissions++;
        response->argument_zero = fixture->submissions;
        memset(fixture->reply, 0, sizeof(fixture->reply));
        put_u32(fixture->reply, fixture->command);
        if (fixture->command == 0) {
            put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
            put_u64(fixture->reply + 8, 1);
            put_u64(fixture->reply + 16, read_u64((const unsigned char *)input + length - 8));
        } else if (fixture->command == 2) {
            fixture->enumerations++;
            uint32_t count = read_u32(bytes + 24);
            put_u32(fixture->reply + 4, (uint32_t)fixture->enumerate_result);
            put_u64(fixture->reply + 8, 1);
            if (!fixture->enumerate_result) {
                put_u32(fixture->reply + 16, fixture->device_count);
                if (count) {
                    assert(count == 2 && length == 88);
                    put_u64(fixture->reply + 20, 2);
                    memcpy(fixture->reply + 28, bytes + 36, 16);
                }
            }
        } else if (fixture->command == 11 || fixture->command == 155) {
            const size_t identity_offset = fixture->command == 11 ? 16 : 12;
            if (fixture->command == 11)
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
            put_u64(fixture->reply + identity_offset - 8, 1);
            put_u64(fixture->reply + identity_offset,
                    read_u64((const unsigned char *)input + length - 8));
            if (fixture->command == 11 && fixture->device_info) {
                unsigned char expected[4096];
                struct instance_encoder_t encoder = {.bytes = expected,
                                                     .capacity = sizeof(expected)};
                VkDevice device = (VkDevice)(uintptr_t)read_u64(fixture->reply + 16);
                vn_encode_vkCreateDevice(&encoder, 1,
                                         (VkPhysicalDevice)(uintptr_t)read_u64(bytes + 8),
                                         fixture->device_info, NULL, &device);
                assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
            }
            if (fixture->command == 155) {
                unsigned char expected[4096];
                struct instance_encoder_t encoder = {.bytes = expected,
                                                     .capacity = sizeof(expected)};
                VkQueue queue = (VkQueue)(uintptr_t)read_u64(bytes + 72);
                VkDeviceQueueTimelineInfoMESA timeline = {
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA,
                    .ringIdx = read_u32(bytes + 48)};
                VkDeviceQueueInfo2 queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                                                 .pNext = &timeline,
                                                 .queueFamilyIndex = read_u32(bytes + 56),
                                                 .queueIndex = read_u32(bytes + 60)};
                vn_encode_vkGetDeviceQueue2(&encoder, 1, (VkDevice)(uintptr_t)read_u64(bytes + 8),
                                            &queue_info, &queue);
                assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
            }
        } else if (fixture->command == 18) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            const uint32_t submit_count = read_u32(bytes + 16);
            assert(submit_count <= 16 && read_u64(bytes + 20) == submit_count);
            VkSubmitInfo infos[16] = {0};
            VkSemaphore waits[16][64], signals[16][64];
            VkPipelineStageFlags stages[16][64];
            VkCommandBuffer buffers[16][64];
            size_t offset = 28;
            for (uint32_t index = 0; index < submit_count; index++) {
                VkSubmitInfo *info = &infos[index];
                assert(read_u32(bytes + offset) == VK_STRUCTURE_TYPE_SUBMIT_INFO &&
                       read_u64(bytes + offset + 4) == 0);
                info->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; offset += 12;
                info->waitSemaphoreCount = read_u32(bytes + offset); offset += 4;
                assert(info->waitSemaphoreCount <= 64 && read_u64(bytes + offset) == info->waitSemaphoreCount);
                offset += 8;
                for (uint32_t item = 0; item < info->waitSemaphoreCount; item++, offset += 8)
                    waits[index][item] = (VkSemaphore)(uintptr_t)read_u64(bytes + offset);
                assert(read_u64(bytes + offset) == info->waitSemaphoreCount); offset += 8;
                for (uint32_t item = 0; item < info->waitSemaphoreCount; item++, offset += 4)
                    stages[index][item] = read_u32(bytes + offset);
                if (info->waitSemaphoreCount) {
                    info->pWaitSemaphores = waits[index]; info->pWaitDstStageMask = stages[index];
                }
                info->commandBufferCount = read_u32(bytes + offset); offset += 4;
                assert(info->commandBufferCount <= 64 && read_u64(bytes + offset) == info->commandBufferCount);
                offset += 8;
                for (uint32_t item = 0; item < info->commandBufferCount; item++, offset += 8)
                    buffers[index][item] = (VkCommandBuffer)(uintptr_t)read_u64(bytes + offset);
                if (info->commandBufferCount) info->pCommandBuffers = buffers[index];
                info->signalSemaphoreCount = read_u32(bytes + offset); offset += 4;
                assert(info->signalSemaphoreCount <= 64 && read_u64(bytes + offset) == info->signalSemaphoreCount);
                offset += 8;
                for (uint32_t item = 0; item < info->signalSemaphoreCount; item++, offset += 8)
                    signals[index][item] = (VkSemaphore)(uintptr_t)read_u64(bytes + offset);
                if (info->signalSemaphoreCount) info->pSignalSemaphores = signals[index];
            }
            fixture->submit_fence = read_u64(bytes + offset); offset += 8;
            assert(offset == length - 36);
            vn_encode_vkQueueSubmit(&encoder, 1, (VkQueue)(uintptr_t)read_u64(bytes + 8),
                                   submit_count, submit_count ? infos : NULL,
                                   (VkFence)(uintptr_t)fixture->submit_fence);
            assert(encoder.used == offset && !memcmp(expected, bytes, offset));
            put_u32(fixture->reply + 4, (uint32_t)fixture->submit_result);
            if (fixture->retire_on_submit) fixture->gpu_pending = 0;
        } else if (fixture->command == 40 || fixture->command == 41) {
            unsigned char expected[128];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 40) {
                VkSemaphore semaphore = (VkSemaphore)(uintptr_t)read_u64(bytes + 56);
                const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                vn_encode_vkCreateSemaphore(&encoder, 1, device, &info, NULL, &semaphore);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, fixture->semaphore_fault == 3 ? 0 : 1);
                put_u64(fixture->reply + 16, fixture->semaphore_fault == 1 || fixture->semaphore_fault == 4 ?
                    0 : (uintptr_t)semaphore + (fixture->semaphore_fault == 2));
            } else {
                vn_encode_vkDestroySemaphore(&encoder, 1, device,
                    (VkSemaphore)(uintptr_t)read_u64(bytes + 16), NULL);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 93 || fixture->command == 103 || fixture->command == 132 || fixture->command == 110) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder={.bytes=expected,.capacity=sizeof(expected)};
            VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)read_u64(bytes+8);
            if(fixture->command==93) vn_encode_vkCmdBindPipeline(&encoder,1,command,read_u32(bytes+16),(VkPipeline)(uintptr_t)read_u64(bytes+20));
            else if(fixture->command==110) vn_encode_vkCmdDispatch(&encoder,1,command,read_u32(bytes+16),read_u32(bytes+20),read_u32(bytes+24));
            else if(fixture->command==132) vn_encode_vkCmdPushConstants(&encoder,1,command,(VkPipelineLayout)(uintptr_t)read_u64(bytes+16),read_u32(bytes+24),read_u32(bytes+28),read_u32(bytes+32),bytes+44);
            else {
                uint32_t count=read_u32(bytes+32);assert(count>0&&count<=16);
                VkDescriptorSet sets[16];for(uint32_t item=0;item<count;++item)sets[item]=(VkDescriptorSet)(uintptr_t)read_u64(bytes+44+8*item);
                assert(read_u32(bytes+44+8*count)==0);
                vn_encode_vkCmdBindDescriptorSets(&encoder,1,command,read_u32(bytes+16),(VkPipelineLayout)(uintptr_t)read_u64(bytes+20),read_u32(bytes+28),count,sets,0,NULL);
            }
            assert(encoder.used==length-36&&!memcmp(expected,bytes,encoder.used));
        } else if (fixture->command == 126) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            uint32_t memory_count = read_u32(bytes + 28);
            assert(memory_count <= 64);
            VkMemoryBarrier memory[64];
            for (uint32_t index = 0; index < memory_count; index++)
                memory[index] = (VkMemoryBarrier){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = read_u32(bytes + 52 + index * 20),
                    .dstAccessMask = read_u32(bytes + 56 + index * 20)};
            size_t buffer_start = 52 + memory_count * 20;
            uint32_t buffer_count = read_u32(bytes + buffer_start - 12);
            assert(buffer_count <= 64);
            VkBufferMemoryBarrier buffers[64];
            for (uint32_t index = 0; index < buffer_count; index++) {
                const unsigned char *record = bytes + buffer_start + index * 52;
                buffers[index] = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .srcAccessMask = read_u32(record + 12), .dstAccessMask = read_u32(record + 16),
                    .srcQueueFamilyIndex = read_u32(record + 20), .dstQueueFamilyIndex = read_u32(record + 24),
                    .buffer = (VkBuffer)(uintptr_t)read_u64(record + 28),
                    .offset = read_u64(record + 36), .size = read_u64(record + 44)};
            }
            size_t image_start = buffer_start + buffer_count * 52 + 12;
            uint32_t image_count = read_u32(bytes + image_start - 12);
            assert(image_count <= 64);
            VkImageMemoryBarrier images[64];
            for (uint32_t index = 0; index < image_count; index++) {
                const unsigned char *record = bytes + image_start + index * 64;
                images[index] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = read_u32(record + 12), .dstAccessMask = read_u32(record + 16),
                    .oldLayout = read_u32(record + 20), .newLayout = read_u32(record + 24),
                    .srcQueueFamilyIndex = read_u32(record + 28), .dstQueueFamilyIndex = read_u32(record + 32),
                    .image = (VkImage)(uintptr_t)read_u64(record + 36),
                    .subresourceRange = {.aspectMask = read_u32(record + 44), .baseMipLevel = read_u32(record + 48),
                        .levelCount = read_u32(record + 52), .baseArrayLayer = read_u32(record + 56), .layerCount = read_u32(record + 60)}};
            }
            vn_encode_vkCmdPipelineBarrier(&encoder, 1,
                (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8), read_u32(bytes + 16),
                read_u32(bytes + 20), read_u32(bytes + 24), memory_count,
                memory_count ? memory : NULL, buffer_count, buffer_count ? buffers : NULL, image_count, image_count ? images : NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 117) {
            unsigned char expected[65584];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            uint64_t size = read_u64(bytes + 32);
            assert(size > 0 && size <= 65536 && size % 4 == 0);
            vn_encode_vkCmdUpdateBuffer(&encoder, 1,
                (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8),
                (VkBuffer)(uintptr_t)read_u64(bytes + 16), read_u64(bytes + 24), size,
                fixture->update_data ? fixture->update_data : bytes + 48);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 112) {
            unsigned char expected[2048];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            uint32_t count = read_u32(bytes + 32);
            assert(count > 0 && count <= 64);
            VkBufferCopy regions[64];
            for (uint32_t index = 0; index < count; index++)
                regions[index] = (VkBufferCopy){read_u64(bytes + 44 + index * 24),
                    read_u64(bytes + 52 + index * 24), read_u64(bytes + 60 + index * 24)};
            vn_encode_vkCmdCopyBuffer(&encoder, 1,
                (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8),
                (VkBuffer)(uintptr_t)read_u64(bytes + 16),
                (VkBuffer)(uintptr_t)read_u64(bytes + 24), count, regions);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 118) {
            unsigned char expected[64];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            vn_encode_vkCmdFillBuffer(&encoder, 1,
                (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8),
                (VkBuffer)(uintptr_t)read_u64(bytes + 16), read_u64(bytes + 24),
                read_u64(bytes + 32), read_u32(bytes + 40));
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command >= 88 && fixture->command <= 92) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            if (fixture->command == 88) {
                assert(fixture->command_allocate);
                VkCommandBufferAllocateInfo info = *fixture->command_allocate;
                info.commandPool = (VkCommandPool)(uintptr_t)read_u64(bytes + 36);
                VkCommandBuffer buffers[64];
                for (uint32_t index = 0; index < info.commandBufferCount; index++)
                    buffers[index] = (VkCommandBuffer)(uintptr_t)read_u64(bytes + 60 + index * 8);
                vn_encode_vkAllocateCommandBuffers(&encoder, 1,
                    (VkDevice)(uintptr_t)read_u64(bytes + 8), &info, buffers);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, fixture->command_array_fault == 1 ?
                        info.commandBufferCount + 1 : info.commandBufferCount);
                for (uint32_t index = 0; index < info.commandBufferCount; index++) {
                    uint64_t id = (uintptr_t)buffers[index];
                    if (fixture->command_array_fault == 2 && index == info.commandBufferCount - 1) id++;
                    if (fixture->command_array_fault == 3 || fixture->command_array_fault == 4) id = 0;
                    put_u64(fixture->reply + 16 + index * 8, id);
                }
            } else if (fixture->command == 89) {
                uint32_t count = read_u32(bytes + 24);
                VkCommandBuffer buffers[64];
                for (uint32_t index = 0; index < count; index++)
                    buffers[index] = (VkCommandBuffer)(uintptr_t)read_u64(bytes + 36 + index * 8);
                vn_encode_vkFreeCommandBuffers(&encoder, 1,
                    (VkDevice)(uintptr_t)read_u64(bytes + 8),
                    (VkCommandPool)(uintptr_t)read_u64(bytes + 16), count, buffers);
            } else {
                VkCommandBuffer buffer = (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8);
                if (fixture->command == 90) {
                    assert(fixture->command_begin);
                    VkCommandBufferBeginInfo info = *fixture->command_begin;
                    if (!read_u64(bytes + 40)) info.pInheritanceInfo = NULL;
                    vn_encode_vkBeginCommandBuffer(&encoder, 1, buffer, &info);
                } else if (fixture->command == 91) {
                    vn_encode_vkEndCommandBuffer(&encoder, 1, buffer);
                } else {
                    vn_encode_vkResetCommandBuffer(&encoder, 1, buffer, read_u32(bytes + 16));
                }
                put_u32(fixture->reply + 4, (uint32_t)fixture->command_result);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command >= 85 && fixture->command <= 87) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 85) {
                VkCommandPool pool = (VkCommandPool)(uintptr_t)read_u64(bytes + length - 44);
                assert(fixture->pool_info);
                vn_encode_vkCreateCommandPool(&encoder, 1, device, fixture->pool_info, NULL, &pool);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1); put_u64(fixture->reply + 16, (uintptr_t)pool);
            } else if (fixture->command == 86) {
                vn_encode_vkDestroyCommandPool(&encoder, 1, device,
                    (VkCommandPool)(uintptr_t)read_u64(bytes + 16), NULL);
            } else {
                vn_encode_vkResetCommandPool(&encoder, 1, device,
                    (VkCommandPool)(uintptr_t)read_u64(bytes + 16), read_u32(bytes + 24));
                put_u32(fixture->reply + 4, (uint32_t)fixture->pool_reset_result);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 21 || fixture->command == 22 || fixture->command == 28) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 21) {
                VkDeviceMemory memory = (VkDeviceMemory)(uintptr_t)read_u64(bytes + length - 44);
                assert(fixture->memory_info);
                vn_encode_vkAllocateMemory(&encoder, 1, device, fixture->memory_info, NULL, &memory);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, (uintptr_t)memory);
            } else if (fixture->command == 22) {
                vn_encode_vkFreeMemory(&encoder, 1, device,
                    (VkDeviceMemory)(uintptr_t)read_u64(bytes + 16), NULL);
            } else {
                vn_encode_vkBindBufferMemory(&encoder, 1, device,
                    (VkBuffer)(uintptr_t)read_u64(bytes + 16),
                    (VkDeviceMemory)(uintptr_t)read_u64(bytes + 24), read_u64(bytes + 32));
                put_u32(fixture->reply + 4, (uint32_t)fixture->bind_result);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 79) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkWriteDescriptorSet writes[64]; VkCopyDescriptorSet copies[64];
            VkDescriptorBufferInfo buffers[64][64];
            size_t cursor = 28;
            assert(read_u32(bytes + 16) == fixture->descriptor_write_count);
            for (uint32_t index = 0; index < fixture->descriptor_write_count; index++) {
                writes[index] = fixture->descriptor_writes[index];
                writes[index].dstSet = (VkDescriptorSet)(uintptr_t)read_u64(bytes + cursor + 12);
                /* Vulkan ignores non-buffer arrays for these buffer descriptor types. */
                writes[index].pImageInfo = NULL; writes[index].pTexelBufferView = NULL;
                for (uint32_t element = 0; element < writes[index].descriptorCount; element++) {
                    buffers[index][element] = writes[index].pBufferInfo[element];
                    buffers[index][element].buffer = (VkBuffer)(uintptr_t)read_u64(bytes + cursor + 52 + element * 24);
                }
                writes[index].pBufferInfo = buffers[index];
                cursor += 60 + 24 * writes[index].descriptorCount;
            }
            assert(read_u32(bytes + cursor) == fixture->descriptor_copy_count);
            cursor += 12;
            for (uint32_t index = 0; index < fixture->descriptor_copy_count; index++) {
                copies[index] = fixture->descriptor_copies[index];
                copies[index].srcSet = (VkDescriptorSet)(uintptr_t)read_u64(bytes + cursor + 12);
                copies[index].dstSet = (VkDescriptorSet)(uintptr_t)read_u64(bytes + cursor + 28);
                cursor += 48;
            }
            vn_encode_vkUpdateDescriptorSets(&encoder, 1, (VkDevice)(uintptr_t)read_u64(bytes + 8),
                fixture->descriptor_write_count, writes, fixture->descriptor_copy_count, copies);
            assert(cursor == length - 36 && encoder.used == cursor && !memcmp(expected, bytes, cursor));
        } else if (fixture->command >= 74 && fixture->command <= 78) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 74) {
                assert(fixture->descriptor_pool_info);
                VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)read_u64(bytes + length - 44);
                vn_encode_vkCreateDescriptorPool(&encoder, 1, device, fixture->descriptor_pool_info, NULL, &pool);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1); put_u64(fixture->reply + 16, (uintptr_t)pool);
            } else if (fixture->command == 75) vn_encode_vkDestroyDescriptorPool(&encoder, 1, device,
                (VkDescriptorPool)(uintptr_t)read_u64(bytes + 16), NULL);
            else if (fixture->command == 76) {
                vn_encode_vkResetDescriptorPool(&encoder, 1, device,
                    (VkDescriptorPool)(uintptr_t)read_u64(bytes + 16), read_u32(bytes + 24));
                put_u32(fixture->reply + 4, (uint32_t)fixture->pool_reset_result);
            } else if (fixture->command == 77) {
                assert(fixture->descriptor_allocate_info);
                VkDescriptorSetAllocateInfo info = *fixture->descriptor_allocate_info;
                info.descriptorPool = (VkDescriptorPool)(uintptr_t)read_u64(bytes + 36);
                VkDescriptorSetLayout layouts[64]; VkDescriptorSet sets[64];
                for (uint32_t index = 0; index < info.descriptorSetCount; index++) {
                    layouts[index] = (VkDescriptorSetLayout)(uintptr_t)read_u64(bytes + 56 + index * 8);
                    sets[index] = (VkDescriptorSet)(uintptr_t)read_u64(bytes + 64 + info.descriptorSetCount * 8 + index * 8);
                }
                info.pSetLayouts = layouts;
                vn_encode_vkAllocateDescriptorSets(&encoder, 1, device, &info, sets);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, info.descriptorSetCount);
                for (uint32_t index = 0; index < info.descriptorSetCount; index++) put_u64(fixture->reply + 16 + index * 8, (uintptr_t)sets[index]);
            } else {
                const uint32_t count = read_u32(bytes + 24);
                VkDescriptorSet sets[64];
                for (uint32_t index = 0; index < count; index++) sets[index] = (VkDescriptorSet)(uintptr_t)read_u64(bytes + 36 + index * 8);
                vn_encode_vkFreeDescriptorSets(&encoder, 1, device,
                    (VkDescriptorPool)(uintptr_t)read_u64(bytes + 16), count, sets);
                put_u32(fixture->reply + 4, (uint32_t)fixture->command_result);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 72 || fixture->command == 73 || fixture->command == 68 || fixture->command == 69) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 72 || fixture->command == 68) {
                uint64_t id = read_u64(bytes + length - 44);
                if (fixture->command == 72) {
                    assert(fixture->descriptor_layout_info);
                    VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)id;
                    vn_encode_vkCreateDescriptorSetLayout(&encoder, 1, device, fixture->descriptor_layout_info, NULL, &layout);
                } else {
                    assert(fixture->pipeline_layout_info);
                    VkPipelineLayoutCreateInfo info = *fixture->pipeline_layout_info;
                    VkDescriptorSetLayout layouts[16];
                    for (uint32_t index = 0; index < info.setLayoutCount; index++)
                        layouts[index] = (VkDescriptorSetLayout)(uintptr_t)read_u64(bytes + 52 + index * 8);
                    info.pSetLayouts = layouts;
                    VkPipelineLayout layout = (VkPipelineLayout)(uintptr_t)id;
                    vn_encode_vkCreatePipelineLayout(&encoder, 1, device, &info, NULL, &layout);
                }
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1); put_u64(fixture->reply + 16, id);
            } else if (fixture->command == 73) vn_encode_vkDestroyDescriptorSetLayout(&encoder, 1, device,
                (VkDescriptorSetLayout)(uintptr_t)read_u64(bytes + 16), NULL);
            else vn_encode_vkDestroyPipelineLayout(&encoder, 1, device,
                (VkPipelineLayout)(uintptr_t)read_u64(bytes + 16), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 80 || fixture->command == 81) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 80) {
                assert(fixture->framebuffer_info && length == 36 + 104);
                VkFramebufferCreateInfo info = *fixture->framebuffer_info;
                info.renderPass = (VkRenderPass)(uintptr_t)read_u64(bytes + 40);
                VkImageView view = (VkImageView)(uintptr_t)read_u64(bytes + 60);
                info.pAttachments = &view;
                VkFramebuffer framebuffer = (VkFramebuffer)(uintptr_t)read_u64(bytes + 96);
                vn_encode_vkCreateFramebuffer(&encoder, 1, device, &info, NULL, &framebuffer);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, (uintptr_t)framebuffer);
            } else vn_encode_vkDestroyFramebuffer(&encoder, 1, device,
                (VkFramebuffer)(uintptr_t)read_u64(bytes + 16), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 82 || fixture->command == 83) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 82) {
                assert(fixture->render_pass_info);
                VkRenderPass pass = (VkRenderPass)(uintptr_t)read_u64(bytes + length - 44);
                vn_encode_vkCreateRenderPass(&encoder, 1, device, fixture->render_pass_info, NULL, &pass);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, (uintptr_t)pass);
            } else vn_encode_vkDestroyRenderPass(&encoder, 1, device,
                (VkRenderPass)(uintptr_t)read_u64(bytes + 16), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 133 || fixture->command == 135) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkCommandBuffer buffer = (VkCommandBuffer)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 133) {
                assert(length == 36 + 116);
                VkClearValue clear = {0};
                for (unsigned index = 0; index < 4; ++index) clear.color.uint32[index] = read_u32(bytes + 96 + index * 4);
                VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                    .renderPass = (VkRenderPass)(uintptr_t)read_u64(bytes + 36),
                    .framebuffer = (VkFramebuffer)(uintptr_t)read_u64(bytes + 44),
                    .renderArea = {{(int32_t)read_u32(bytes + 52), (int32_t)read_u32(bytes + 56)}, {read_u32(bytes + 60), read_u32(bytes + 64)}},
                    .clearValueCount = 1, .pClearValues = &clear};
                vn_encode_vkCmdBeginRenderPass(&encoder, 1, buffer, &info, read_u32(bytes + 112));
            } else vn_encode_vkCmdEndRenderPass(&encoder, 1, buffer);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 65) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            assert(fixture->graphics_info && length == 36 + 632);
            VkGraphicsPipelineCreateInfo info = *fixture->graphics_info;
            VkPipelineShaderStageCreateInfo stages[2] = {info.pStages[0], info.pStages[1]};
            stages[0].module = (VkShaderModule)(uintptr_t)read_u64(bytes + 84);
            stages[1].module = (VkShaderModule)(uintptr_t)read_u64(bytes + 136);
            info.pStages = stages;
            info.layout = (VkPipelineLayout)(uintptr_t)read_u64(bytes + 576);
            info.renderPass = (VkRenderPass)(uintptr_t)read_u64(bytes + 584);
            VkPipeline pipeline = (VkPipeline)(uintptr_t)read_u64(bytes + 624);
            vn_encode_vkCreateGraphicsPipelines(&encoder, 1, (VkDevice)(uintptr_t)read_u64(bytes + 8), NULL, 1, &info, NULL, &pipeline);
            put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
            put_u64(fixture->reply + 8, 1);
            put_u64(fixture->reply + 16, (uintptr_t)pipeline);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 66 || fixture->command == 67) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 66) {
                assert(fixture->compute_info);
                VkComputePipelineCreateInfo info = *fixture->compute_info;
                info.stage.module = (VkShaderModule)(uintptr_t)read_u64(bytes + 72);
                info.layout = (VkPipelineLayout)(uintptr_t)read_u64(bytes + length - 80);
                VkPipeline pipeline = (VkPipeline)(uintptr_t)read_u64(bytes + length - 44);
                vn_encode_vkCreateComputePipelines(&encoder, 1, device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1); put_u64(fixture->reply + 16, (uintptr_t)pipeline);
            } else vn_encode_vkDestroyPipeline(&encoder, 1, device,
                (VkPipeline)(uintptr_t)read_u64(bytes + 16), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 59 || fixture->command == 60) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 59) {
                assert(fixture->shader_info);
                VkShaderModule shader = (VkShaderModule)(uintptr_t)read_u64(bytes + length - 44);
                vn_encode_vkCreateShaderModule(&encoder, 1, device, fixture->shader_info, NULL, &shader);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, (uintptr_t)shader);
            } else vn_encode_vkDestroyShaderModule(&encoder, 1, device,
                (VkShaderModule)(uintptr_t)read_u64(bytes + 16), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 54 || fixture->command == 55 || fixture->command == 57 || fixture->command == 58 || fixture->command == 31 || fixture->command == 29) {
            unsigned char expected[8192];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 54 || fixture->command == 57) {
                uint64_t id = read_u64(bytes + length - 44);
                if (fixture->command == 54) {
                    assert(fixture->image_info);
                    VkImageCreateInfo info = *fixture->image_info;
                    if (info.sharingMode == VK_SHARING_MODE_EXCLUSIVE) info.queueFamilyIndexCount = 0;
                    VkImage image = (VkImage)(uintptr_t)id;
                    vn_encode_vkCreateImage(&encoder, 1, device, &info, NULL, &image);
                } else {
                    assert(fixture->view_info);
                    VkImageViewCreateInfo info = *fixture->view_info;
                    info.image = (VkImage)(uintptr_t)read_u64(bytes + 40);
                    VkImageView view = (VkImageView)(uintptr_t)id;
                    vn_encode_vkCreateImageView(&encoder, 1, device, &info, NULL, &view);
                }
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, id);
            } else if (fixture->command == 55) {
                vn_encode_vkDestroyImage(&encoder, 1, device, (VkImage)(uintptr_t)read_u64(bytes + 16), NULL);
            } else if (fixture->command == 58) {
                vn_encode_vkDestroyImageView(&encoder, 1, device, (VkImageView)(uintptr_t)read_u64(bytes + 16), NULL);
            } else if (fixture->command == 29) {
                vn_encode_vkBindImageMemory(&encoder, 1, device, (VkImage)(uintptr_t)read_u64(bytes + 16),
                    (VkDeviceMemory)(uintptr_t)read_u64(bytes + 24), read_u64(bytes + 32));
                put_u32(fixture->reply + 4, (uint32_t)fixture->bind_result);
            } else {
                VkMemoryRequirements value = {0};
                vn_encode_vkGetImageMemoryRequirements(&encoder, 1, device, (VkImage)(uintptr_t)read_u64(bytes + 16), &value);
                put_u64(fixture->reply + 4, 1);
                put_u64(fixture->reply + 12, fixture->requirements_fault == 2 ? 0 : fixture->requirements_size);
                put_u64(fixture->reply + 20, fixture->requirements_fault == 3 ? 0 : fixture->requirements_fault == 4 ? 3 : 256);
                put_u32(fixture->reply + 28, fixture->requirements_fault == 5 ? 0 : 7);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 50 || fixture->command == 51 || fixture->command == 30) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 50) {
                VkBuffer buffer = (VkBuffer)(uintptr_t)read_u64(bytes + length - 44);
                assert(fixture->buffer_info);
                VkBufferCreateInfo info = *fixture->buffer_info;
                if (info.sharingMode == VK_SHARING_MODE_EXCLUSIVE) info.queueFamilyIndexCount = 0;
                vn_encode_vkCreateBuffer(&encoder, 1, device, &info, NULL, &buffer);
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, (uintptr_t)buffer);
            } else if (fixture->command == 51) {
                vn_encode_vkDestroyBuffer(&encoder, 1, device,
                                          (VkBuffer)(uintptr_t)read_u64(bytes + 16), NULL);
            } else {
                VkMemoryRequirements value = {0};
                vn_encode_vkGetBufferMemoryRequirements(&encoder, 1, device,
                                          (VkBuffer)(uintptr_t)read_u64(bytes + 16), &value);
                put_u64(fixture->reply + 4, fixture->requirements_fault == 1 ? 0 : 1);
                put_u64(fixture->reply + 12, fixture->requirements_fault == 2 ? 0 :
                    fixture->requirements_fault == 6 ? 1 : fixture->requirements_size);
                put_u64(fixture->reply + 20, fixture->requirements_fault == 3 ? 0 :
                                            fixture->requirements_fault == 4 ? 3 : 256);
                put_u32(fixture->reply + 28, fixture->requirements_fault == 5 ? 0 : 7);
            }
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command >= 35 && fixture->command <= 39) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            VkDevice device = (VkDevice)(uintptr_t)read_u64(bytes + 8);
            if (fixture->command == 35) {
                VkFence fence = (VkFence)(uintptr_t)read_u64(bytes + 56);
                VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                          .flags = read_u32(bytes + 36)};
                vn_encode_vkCreateFence(&encoder, 1, device, &info, NULL, &fence);
                uint64_t id = (uintptr_t)fence;
                assert(id < 4096);
                fixture->fence_ready[id] = !!info.flags;
                put_u32(fixture->reply + 4, (uint32_t)fixture->create_result);
                put_u64(fixture->reply + 8, 1);
                put_u64(fixture->reply + 16, id);
            } else if (fixture->command == 36 || fixture->command == 38) {
                VkFence fence = (VkFence)(uintptr_t)read_u64(bytes + 16);
                assert((uintptr_t)fence < 4096);
                if (fixture->command == 36) {
                    vn_encode_vkDestroyFence(&encoder, 1, device, fence, NULL);
                    fixture->fence_ready[(uintptr_t)fence] = 0;
                } else {
                    vn_encode_vkGetFenceStatus(&encoder, 1, device, fence);
                    put_u32(fixture->reply + 4,
                            fixture->fence_ready[(uintptr_t)fence] ? VK_SUCCESS : VK_NOT_READY);
                }
            } else {
                const uint32_t count = read_u32(bytes + 16);
                assert(count > 0 && count <= 64 && read_u64(bytes + 20) == count);
                VkFence fences[64];
                uint32_t ready = 0;
                for (uint32_t index = 0; index < count; index++) {
                    uint64_t id = read_u64(bytes + 28 + index * 8);
                    assert(id < 4096);
                    fences[index] = (VkFence)(uintptr_t)id;
                    ready += fixture->fence_ready[id];
                    if (fixture->command == 37)
                        fixture->fence_ready[id] = 0;
                }
                if (fixture->command == 37)
                    vn_encode_vkResetFences(&encoder, 1, device, count, fences);
                else {
                    const uint32_t all = read_u32(bytes + 28 + count * 8);
                    assert(all <= 1 && read_u64(bytes + 32 + count * 8) == 0);
                    vn_encode_vkWaitForFences(&encoder, 1, device, count, fences, all, 0);
                    int32_t result = (all ? ready == count : ready > 0) ? VK_SUCCESS : VK_TIMEOUT;
                    if (fixture->fence_pending) {
                        fixture->fence_pending--;
                        result = VK_TIMEOUT;
                    }
                    put_u32(fixture->reply + 4, (uint32_t)result);
                }
            }
            if (fixture->fence_override)
                put_u32(fixture->reply + 4, (uint32_t)fixture->fence_result);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command == 12) {
            unsigned char expected[4096];
            struct instance_encoder_t encoder = {.bytes = expected, .capacity = sizeof(expected)};
            vn_encode_vkDestroyDevice(&encoder, 1, (VkDevice)(uintptr_t)read_u64(bytes + 8), NULL);
            assert(encoder.used == length - 36 && !memcmp(expected, bytes, encoder.used));
        } else if (fixture->command != 1) {
            assert(
                venus_values_test_encode(fixture->command, fixture->reply, sizeof(fixture->reply)));
            if ((fixture->command == 7 && read_u32(bytes + 24) == 0) ||
                (fixture->command == 33 && read_u32(bytes + 44) == 0))
                put_u64(fixture->reply + 16, 0);
        }
        if (fixture->command == 8 && fixture->mapping_enabled && !fixture->mapping_noncoherent)
            put_u32(fixture->reply + 24, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (fixture->command == 6 && fixture->descriptor_properties) {
            VkPhysicalDeviceProperties properties = {.apiVersion = VK_API_VERSION_1_0};
            properties.limits.minUniformBufferOffsetAlignment = 16;
            properties.limits.minStorageBufferOffsetAlignment = 32;
            properties.limits.maxUniformBufferRange = 256;
            properties.limits.maxStorageBufferRange = 4096;
            const uint32_t PushLimits[] = {128,128,256,512,0,126,127,129};
            assert(fixture->push_limit_mode < sizeof(PushLimits)/sizeof(PushLimits[0]));
            properties.limits.maxPushConstantsSize = PushLimits[fixture->push_limit_mode];
            for (unsigned axis = 0; axis < 3; axis++) properties.limits.maxComputeWorkGroupCount[axis] = 8;
            if (fixture->descriptor_properties == 2) properties.limits.minUniformBufferOffsetAlignment = 0;
            if (fixture->descriptor_properties == 3) properties.limits.minStorageBufferOffsetAlignment = 3;
            if (fixture->descriptor_properties == 4) properties.limits.maxStorageBufferRange = 0;
            if (fixture->descriptor_properties == 5) properties.limits.maxComputeWorkGroupCount[2] = 0;
            assert(venus_values_test_properties(&properties, fixture->reply, sizeof(fixture->reply)));
        }
        if (fixture->command == 6 && fixture->properties_override)
            put_u32(fixture->reply + 12, fixture->properties_version);
        if (fixture->command == fixture->corrupt_command) {
            if (fixture->command == 0)
                fixture->reply[16] ^= 1;
            else if (fixture->command == 2)
                fixture->reply[8] = 0;
            else if (fixture->command == 3 || fixture->command == 6 || fixture->command == 8)
                fixture->reply[4] = 0;
            else
                fixture->reply[0] ^= 1;
        }
    } else if (request->kind == RequestPoll) {
        assert(!input && !length && !output && !capacity);
        if (fixture->poll_again) {
            fixture->poll_again--;
            return RingAgain;
        }
    } else {
        assert(request->kind == RequestReply && !input && !length && output &&
               capacity == sizeof(fixture->reply));
        if (fixture->reply_again) {
            fixture->reply_again--;
            return RingAgain;
        }
        // Native void replies initialize only their opcode; trailing scratch is unspecified.
        if (fixture->command == 55 || fixture->command == 58) fixture->reply[123] = 0xa5;
        memcpy(output, fixture->reply, capacity);
        response->payload_bytes = (uint32_t)capacity;
    }
    return RingOk;
}
static fixture_t fresh(void) {
    return (fixture_t){.fail_command = UINT32_MAX,
                       .corrupt_command = UINT32_MAX,
                       .transport_failure = RingClosed,
                       .device_count = 2, .requirements_size = 8192};
}
static PFN_vkCreateInstance create_function(void) {
    return (PFN_vkCreateInstance)lookup_external(NULL, "vkCreateInstance");
}
static VkInstance create(void) {
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = NULL;
    assert(create_function()(&info, NULL, &instance) == VK_SUCCESS && instance);
    return instance;
}
static void destroy(VkInstance instance) {
    PFN_vkDestroyInstance function =
        (PFN_vkDestroyInstance)lookup_external(instance, "vkDestroyInstance");
    assert(function);
    function(instance, NULL);
}
static void routing(void) {
    uint32_t version = 9;
    assert(negotiate_external(&version) == VK_SUCCESS && version == 5);
    for (version = 2; version <= 5; version++) {
        uint32_t input = version;
        assert(venus_icd_negotiate_loader(&input) == VK_SUCCESS && input == version);
    }
    version = 1;
    assert(negotiate_external(&version) == VK_ERROR_INITIALIZATION_FAILED && version == 1);
    assert(negotiate_external(NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(!lookup_external(NULL, NULL));
    char long_name[256];
    memset(long_name, 'a', sizeof(long_name));
    assert(!lookup_external(NULL, long_name));
    assert(!lookup_external(NULL, "vkDestroyInstance"));
    assert(!lookup_external(NULL, "vkCreateDevice"));
    assert(!lookup_external((VkInstance)(uintptr_t)1, "vkCreateInstance"));
    PFN_vkEnumerateInstanceVersion enumerate =
        (PFN_vkEnumerateInstanceVersion)lookup_external(NULL, "vkEnumerateInstanceVersion");
    assert(enumerate && enumerate(&version) == VK_SUCCESS && version == VK_API_VERSION_1_0);
    assert(enumerate(NULL) == VK_ERROR_INITIALIZATION_FAILED);
    PFN_vkEnumerateInstanceExtensionProperties extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)lookup_external(
            NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t count = 99;
    assert(extensions(NULL, &count, NULL) == VK_SUCCESS && !count);
    assert(extensions("missing", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    assert(extensions(NULL, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
}
static void healthy(fixture_t *fixture) {
    assert(venus_icd_bind(exchange, fixture) == RingOk);
    assert(venus_icd_bind(exchange, fixture) == RingInvalid);
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        VkInstance instance = create();
        assert(venus_icd_unbind() == RingAgain);
        assert(!lookup_external(instance, "vkUnknown"));
        assert(lookup_external(instance, "vkCreateDevice"));
        assert(!venus_icd_get_physical_proc_addr(instance, "vkCreateInstance"));
        assert(venus_icd_get_physical_proc_addr(instance, "vkGetPhysicalDeviceProperties"));
        assert(!venus_icd_get_physical_proc_addr(NULL, "vkGetPhysicalDeviceProperties"));
        assert(!venus_icd_get_physical_proc_addr(instance, NULL));
        assert(!venus_icd_get_physical_proc_addr((VkInstance)(uintptr_t)1,
                                                 "vkGetPhysicalDeviceProperties"));
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 99;
        unsigned before = fixture->enumerations;
        assert(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        VkPhysicalDevice devices[2] = {0};
        count = 1;
        assert(enumerate(instance, &count, devices) == VK_INCOMPLETE && count == 1);
        VkPhysicalDevice first = devices[0];
        count = 0;
        assert(enumerate(instance, &count, devices) == VK_INCOMPLETE && !count);
        count = 2;
        assert(enumerate(instance, &count, devices) == VK_SUCCESS && count == 2);
        assert(devices[0] == first && fixture->enumerations == before + 2);
        VkPhysicalDeviceProperties properties = {0};
        VkPhysicalDeviceFeatures features = {0};
        VkPhysicalDeviceMemoryProperties memory = {0};
        PFN_vkGetPhysicalDeviceProperties get_properties =
            (PFN_vkGetPhysicalDeviceProperties)lookup_external(instance,
                                                               "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceFeatures get_features =
            (PFN_vkGetPhysicalDeviceFeatures)lookup_external(instance,
                                                             "vkGetPhysicalDeviceFeatures");
        PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
            (PFN_vkGetPhysicalDeviceMemoryProperties)lookup_external(
                instance, "vkGetPhysicalDeviceMemoryProperties");
        get_properties(devices[0], &properties);
        get_features(devices[0], &features);
        get_memory(devices[0], &memory);
        PFN_vkGetPhysicalDeviceFormatProperties format =
            (PFN_vkGetPhysicalDeviceFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceFormatProperties");
        VkFormatProperties format_value = {0};
        format(devices[0], VK_FORMAT_R8G8B8A8_UNORM, NULL);
        format(NULL, VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        format(devices[0], VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        assert(format_value.optimalTilingFeatures == VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        format(devices[0], VK_FORMAT_R8G8B8A8_UNORM, NULL);
        format(NULL, VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        format((VkPhysicalDevice)(uintptr_t)1, VK_FORMAT_R8G8B8A8_UNORM, &format_value);
        PFN_vkGetPhysicalDeviceImageFormatProperties image =
            (PFN_vkGetPhysicalDeviceImageFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceImageFormatProperties");
        VkImageFormatProperties image_value = {0};
        assert(image(NULL, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_value) == VK_ERROR_DEVICE_LOST);
        assert(image(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                     &image_value) == VK_SUCCESS);
        assert(image_value.maxExtent.width == 4096 && image_value.maxResourceSize == 1048576);
        assert(image(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                     NULL) == VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkGetPhysicalDeviceQueueFamilyProperties queues =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)lookup_external(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        count = 0;
        queues(devices[0], &count, NULL);
        assert(count == 1);
        VkQueueFamilyProperties queue = {0};
        queues(devices[0], &count, &queue);
        assert(count == 1 && queue.queueCount == 1 && queue.timestampValidBits == 64);
        queues(devices[0], NULL, NULL);
        queues(NULL, &count, &queue);
        count = 65;
        queues(devices[0], &count, &queue);
        count = 0;
        queues(devices[0], &count, &queue);
        assert(count == 0);
        PFN_vkGetPhysicalDeviceSparseImageFormatProperties sparse =
            (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)lookup_external(
                instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
        count = 0;
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, NULL);
        assert(count == 1);
        VkSparseImageFormatProperties sparse_value = {0};
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, &sparse_value);
        assert(count == 1 && sparse_value.imageGranularity.width == 64);
        sparse(NULL, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &count, &sparse_value);
        sparse(devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, NULL, NULL);
        PFN_vkEnumerateDeviceExtensionProperties device_extensions =
            (PFN_vkEnumerateDeviceExtensionProperties)lookup_external(
                instance, "vkEnumerateDeviceExtensionProperties");
        assert(device_extensions(devices[0], NULL, &count, NULL) == VK_SUCCESS && count == 0);
        assert(device_extensions(NULL, NULL, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(device_extensions((VkPhysicalDevice)(uintptr_t)1, NULL, &count, NULL) ==
               VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkCreateDevice device_create =
            (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        VkDevice device_handle = (VkDevice)(uintptr_t)1;
        assert(device_create(devices[0], NULL, NULL, &device_handle) ==
                   VK_ERROR_INITIALIZATION_FAILED &&
               !device_handle);
        float priorities[2] = {0.25f, 0.75f};
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = 0,
                                              .queueCount = 2,
                                              .pQueuePriorities = priorities};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue_info};
        fixture->device_info = &device_info;
        assert(device_create(devices[0], &device_info, NULL, &device_handle) == VK_SUCCESS);
        fixture->device_info = NULL;
        assert(device_handle);
        PFN_vkGetDeviceProcAddr device_lookup =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        assert(!device_lookup(NULL, "vkDestroyDevice"));
        assert(!device_lookup((VkDevice)(uintptr_t)1, "vkDestroyDevice"));
        assert(!device_lookup(device_handle, NULL));
        assert(device_lookup(device_handle, "vkCreateBuffer"));
        PFN_vkGetDeviceQueue get_queue =
            (PFN_vkGetDeviceQueue)device_lookup(device_handle, "vkGetDeviceQueue");
        PFN_vkDestroyDevice device_destroy =
            (PFN_vkDestroyDevice)device_lookup(device_handle, "vkDestroyDevice");
        PFN_vkDeviceWaitIdle device_idle =
            (PFN_vkDeviceWaitIdle)device_lookup(device_handle, "vkDeviceWaitIdle");
        PFN_vkQueueWaitIdle queue_idle =
            (PFN_vkQueueWaitIdle)device_lookup(device_handle, "vkQueueWaitIdle");
        assert(get_queue && device_destroy && device_idle && queue_idle);
        assert(device_idle(device_handle) == VK_SUCCESS);
        assert(lookup_external(instance, "vkDestroyDevice"));
        VkQueue first_queue = NULL, second = NULL;
        get_queue(device_handle, 0, 0, &first_queue);
        get_queue(device_handle, 0, 1, &second);
        assert(first_queue && second && first_queue != second);
        VkQueue repeat = NULL;
        get_queue(device_handle, 0, 0, &repeat);
        assert(repeat == first_queue);
        get_queue(device_handle, 1, 0, &repeat);
        assert(!repeat);
        get_queue(device_handle, 0, 2, &repeat);
        assert(!repeat);
        get_queue(NULL, 0, 0, &repeat);
        get_queue((VkDevice)(uintptr_t)1, 0, 0, &repeat);
        get_queue(device_handle, 0, 0, NULL);
        fixture->gpu_pending = 1;
        fixture->gpu_issue_pending = 1;
        assert(queue_idle(first_queue) == VK_SUCCESS);
        assert(device_idle(device_handle) == VK_SUCCESS);
        assert(queue_idle(NULL) == VK_ERROR_DEVICE_LOST);
        assert(queue_idle((VkQueue)(uintptr_t)1) == VK_ERROR_DEVICE_LOST);
        assert(device_idle(NULL) == VK_ERROR_DEVICE_LOST);
        assert(device_idle((VkDevice)(uintptr_t)1) == VK_ERROR_DEVICE_LOST);
        PFN_vkCreateFence fence_create =
            (PFN_vkCreateFence)device_lookup(device_handle, "vkCreateFence");
        PFN_vkDestroyFence fence_destroy =
            (PFN_vkDestroyFence)device_lookup(device_handle, "vkDestroyFence");
        PFN_vkResetFences fence_reset =
            (PFN_vkResetFences)device_lookup(device_handle, "vkResetFences");
        PFN_vkGetFenceStatus fence_status =
            (PFN_vkGetFenceStatus)device_lookup(device_handle, "vkGetFenceStatus");
        PFN_vkWaitForFences fence_wait =
            (PFN_vkWaitForFences)device_lookup(device_handle, "vkWaitForFences");
        assert(fence_create && fence_destroy && fence_reset && fence_status && fence_wait);
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                        .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fences[2] = {0};
        assert(fence_create(device_handle, &fence_info, NULL, &fences[0]) == VK_SUCCESS);
        fence_info.flags = 0;
        assert(fence_create(device_handle, &fence_info, NULL, &fences[1]) == VK_SUCCESS);
        assert(fences[0] && fences[1] && fences[0] != fences[1]);
        assert(fence_status(device_handle, fences[0]) == VK_SUCCESS);
        assert(fence_status(device_handle, fences[1]) == VK_NOT_READY);
        assert(fence_wait(device_handle, 2, fences, VK_FALSE, 0) == VK_SUCCESS);
        assert(fence_wait(device_handle, 2, fences, VK_TRUE, 0) == VK_TIMEOUT);
        assert(fence_wait(device_handle, 1, fences + 1, VK_FALSE, 1000000) == VK_TIMEOUT);
        fixture->fence_pending = 1;
        assert(fence_wait(device_handle, 1, fences, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        device_destroy(device_handle, NULL); /* Live nonqueue children block device retirement. */
        assert(device_lookup(device_handle, "vkDestroyDevice"));
        assert(fence_reset(device_handle, 2, fences) == VK_SUCCESS);
        assert(fence_status(device_handle, fences[0]) == VK_NOT_READY);
        assert(fence_wait(device_handle, 2, fences, VK_FALSE, 0) == VK_TIMEOUT);
        fence_destroy(device_handle, fences[0], NULL);
        assert(fence_status(device_handle, fences[0]) == VK_ERROR_DEVICE_LOST);
        fence_destroy(device_handle, fences[0], NULL);
        fence_destroy(device_handle, fences[1], NULL);
        destroy(instance); /* Parent remains live until its device is retired. */
        assert(lookup_external(instance, "vkDestroyInstance"));
        device_destroy(NULL, NULL);
        device_destroy((VkDevice)(uintptr_t)1, NULL);
        device_destroy(device_handle, NULL);
        assert(!device_lookup(device_handle, "vkDestroyDevice"));
        assert(queue_idle(first_queue) == VK_ERROR_DEVICE_LOST);
        get_queue(device_handle, 0, 0, &repeat);
        assert(!repeat);
        device_destroy(device_handle, NULL);
        assert(properties.apiVersion == VK_API_VERSION_1_0 && properties.vendorID == 42 &&
               features.robustBufferAccess && memory.memoryTypeCount);
        get_properties((VkPhysicalDevice)(uintptr_t)1, &properties);
        get_properties(NULL, &properties);
        get_properties(devices[0], NULL);
        get_features(devices[0], NULL);
        get_features(NULL, &features);
        get_memory(devices[0], NULL);
        get_memory(NULL, &memory);
        assert(enumerate(NULL, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(enumerate((VkInstance)(uintptr_t)1, &count, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(enumerate(instance, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        PFN_vkGetDeviceProcAddr device =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        assert(device && !device(NULL, "vkCreateBuffer"));
        PFN_vkDestroyInstance retire =
            (PFN_vkDestroyInstance)lookup_external(instance, "vkDestroyInstance");
        retire(NULL, NULL);
        retire((VkInstance)(uintptr_t)1, NULL);
        destroy(instance);
        assert(!lookup_external(instance, "vkDestroyInstance"));
        retire(instance, NULL);
    }
    assert(venus_icd_unbind() == RingOk);
}
#ifdef _WIN32
static DWORD WINAPI stress_thread(void *argument) {
#else
static void *stress_thread(void *argument) {
#endif
    (void)argument;
    for (unsigned iteration = 0; iteration < 32; iteration++) {
        VkInstance instance = create();
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        assert(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        destroy(instance);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void concurrent(void) {
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
#ifdef _WIN32
    HANDLE threads[4];
    for (unsigned index = 0; index < 4; index++) {
        threads[index] = CreateThread(NULL, 0, stress_thread, NULL, 0, NULL);
        assert(threads[index]);
    }
    for (unsigned index = 0; index < 4; index++) {
        assert(WaitForSingleObject(threads[index], 30000) == WAIT_OBJECT_0);
        DWORD result;
        assert(GetExitCodeThread(threads[index], &result) && result == 0);
        assert(CloseHandle(threads[index]));
    }
#else
    pthread_t threads[4];
    for (unsigned index = 0; index < 4; index++)
        assert(pthread_create(&threads[index], NULL, stress_thread, NULL) == 0);
    for (unsigned index = 0; index < 4; index++)
        assert(pthread_join(threads[index], NULL) == 0);
#endif
    assert(venus_icd_unbind() == RingOk);
}
static void fence_failures(void) {
    for (unsigned scenario = 0; scenario < 17; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        VkPhysicalDevice physical[2];
        uint32_t count = 2;
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
                   instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) ==
               VK_SUCCESS);
        float priority = 0.5f;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                   .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &queue_info};
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device =
            (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateFence create_fence = (PFN_vkCreateFence)lookup(device, "vkCreateFence");
        PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)lookup(device, "vkDestroyFence");
        PFN_vkResetFences reset_fences = (PFN_vkResetFences)lookup(device, "vkResetFences");
        PFN_vkGetFenceStatus status = (PFN_vkGetFenceStatus)lookup(device, "vkGetFenceStatus");
        PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)lookup(device, "vkWaitForFences");
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                        .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence invalid = NULL, fence = NULL;
        assert(create_fence(device, &fence_info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_fence(NULL, &fence_info, NULL, &invalid) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_fence((VkDevice)(uintptr_t)1, &fence_info, NULL, &invalid) ==
               VK_ERROR_INITIALIZATION_FAILED);
        assert(create_fence(device, NULL, NULL, &invalid) == VK_ERROR_INITIALIZATION_FAILED);
        fence_info.sType = 0;
        assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_INITIALIZATION_FAILED);
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = 2;
        assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_INITIALIZATION_FAILED);
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        fence_info.pNext = &info;
        assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_INITIALIZATION_FAILED);
        fence_info.pNext = NULL;
        fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
               !invalid);
        fixture.create_result = VK_SUCCESS;
        assert(create_fence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
        assert(status(foreign, fence) == VK_ERROR_DEVICE_LOST);
        assert(status(NULL, fence) == VK_ERROR_DEVICE_LOST);
        assert(status(device, NULL) == VK_ERROR_DEVICE_LOST);
        assert(status((VkDevice)(uintptr_t)1, fence) == VK_ERROR_DEVICE_LOST);
        assert(status(device, (VkFence)device) == VK_ERROR_DEVICE_LOST);
        destroy_fence(foreign, fence, NULL);
        destroy_fence(NULL, fence, NULL);
        destroy_fence(device, NULL, NULL);
        destroy_fence((VkDevice)(uintptr_t)1, fence, NULL);
        assert(status(device, fence) == VK_SUCCESS);
        assert(reset_fences(foreign, 1, &fence) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(NULL, 1, &fence) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(device, 0, &fence) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(device, 65, &fence) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(device, 1, NULL) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(device, 1, &invalid) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(device, 0, &fence, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(device, 65, &fence, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(device, 1, NULL, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(device, 1, &fence, 2, 0) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(foreign, 1, &fence, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        const uint32_t command = 35 + scenario % 5;
        if (scenario < 5)
            fixture.corrupt_command = command;
        else if (scenario < 10)
            fixture.fail_command = command;
        else {
            fixture.fence_override = 1;
            fixture.fence_result = scenario == 10 ? VK_ERROR_DEVICE_LOST : VK_INCOMPLETE;
        }
        if (scenario >= 12) {
            fixture.fence_override = 0;
            fixture.create_result = scenario == 12 ? VK_ERROR_DEVICE_LOST : VK_NOT_READY;
            assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_DEVICE_LOST);
        } else if (scenario >= 10)
            assert(status(device, fence) == VK_ERROR_DEVICE_LOST);
        else if (command == 35)
            assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_DEVICE_LOST &&
                   !invalid);
        else if (command == 36)
            destroy_fence(device, fence, NULL);
        else if (command == 37)
            assert(reset_fences(device, 1, &fence) == VK_ERROR_DEVICE_LOST);
        else if (command == 38)
            assert(status(device, fence) == VK_ERROR_DEVICE_LOST);
        else
            assert(wait_fences(device, 1, &fence, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        assert(status(device, fence) == VK_ERROR_DEVICE_LOST);
        assert(reset_fences(device, 1, &fence) == VK_ERROR_DEVICE_LOST);
        assert(wait_fences(device, 1, &fence, VK_TRUE, 0) == VK_ERROR_DEVICE_LOST);
        assert(create_fence(device, &fence_info, NULL, &invalid) == VK_ERROR_DEVICE_LOST);
        assert(venus_icd_unbind() == RingAgain);
        venus_icd_abandon();
    }
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkInstance instance = create();
    VkPhysicalDevice physical[2];
    uint32_t count = 2;
    assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
               instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
    float priority = 0.5f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue};
    VkDevice device = NULL;
    assert(((PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice"))(
               physical[0], &info, NULL, &device) == VK_SUCCESS);
    PFN_vkGetDeviceProcAddr lookup =
        (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
    PFN_vkCreateFence create_fence = (PFN_vkCreateFence)lookup(device, "vkCreateFence");
    PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)lookup(device, "vkDestroyFence");
    PFN_vkResetFences reset_fences = (PFN_vkResetFences)lookup(device, "vkResetFences");
    PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)lookup(device, "vkWaitForFences");
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                    .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence fences[507], extra = NULL;
    for (unsigned index = 0; index < 507; index++)
        assert(create_fence(device, &fence_info, NULL, &fences[index]) == VK_SUCCESS);
    assert(create_fence(device, &fence_info, NULL, &extra) == VK_ERROR_OUT_OF_HOST_MEMORY &&
           !extra);
    VkQueue reserved_queue = NULL;
    ((PFN_vkGetDeviceQueue)lookup(device, "vkGetDeviceQueue"))(device,0,0,&reserved_queue);
    assert(reserved_queue);
    destroy_fence(device,fences[506],NULL);
    VkDevice rejected = NULL;
    unsigned before = fixture.submissions;
    assert(((PFN_vkCreateDevice)lookup_external(instance,"vkCreateDevice"))(
        physical[0],&info,NULL,&rejected) == VK_ERROR_OUT_OF_HOST_MEMORY && !rejected);
    assert(fixture.submissions == before);
    assert(create_fence(device,&fence_info,NULL,&fences[506]) == VK_SUCCESS);
    assert(wait_fences(device, 64, fences, VK_TRUE, 0) == VK_SUCCESS);
    assert(reset_fences(device, 64, fences) == VK_SUCCESS);
    for (unsigned index = 0; index < 507; index++)
        destroy_fence(device, fences[index], NULL);
    ((PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice"))(device, NULL);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
}
/** @brief Borrowed idle thread inputs; result read only after native thread join. */
typedef struct idle_thread_t {
    VkQueue queue; /**< Live externally synchronized private queue. */
    PFN_vkQueueWaitIdle idle; /**< Static borrowed native function. */
    VkResult result; /**< Thread writes once; join publishes to controller. */
} idle_thread_t;
#ifdef _WIN32
static DWORD WINAPI idle_thread(void *argument) {
#else
static void *idle_thread(void *argument) {
#endif
    idle_thread_t *state = argument;
    state->result = state->idle(state->queue);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void idle_concurrency_contract(void) {
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        fixture_t fixture = fresh(), replacement = fresh();
        assert(venus_icd_bind(exchange,&fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2; VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,"vkEnumeratePhysicalDevices"))(
            instance,&count,physical) == VK_SUCCESS);
        const float priorities[2] = {1,1};
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 2, .pQueuePriorities = priorities};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
        fixture.device_info = &info;
        VkDevice device = NULL;
        assert(((PFN_vkCreateDevice)lookup_external(instance,"vkCreateDevice"))(
            physical[0],&info,NULL,&device) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance,"vkGetDeviceProcAddr");
        PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)lookup(device,"vkGetDeviceQueue");
        PFN_vkQueueWaitIdle idle = (PFN_vkQueueWaitIdle)lookup(device,"vkQueueWaitIdle");
        PFN_vkDeviceWaitIdle device_idle = (PFN_vkDeviceWaitIdle)lookup(device,"vkDeviceWaitIdle");
        PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)lookup(device,"vkQueueSubmit");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device,"vkDestroyDevice");
        VkQueue queues[2] = {NULL,NULL};
        get_queue(device,0,0,&queues[0]); get_queue(device,0,1,&queues[1]);
        fixture.gpu_pending = UINT32_MAX; fixture.retire_on_submit = 1;
        idle_thread_t state = {.queue = queues[0], .idle = idle, .result = VK_NOT_READY};
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL,0,idle_thread,&state,0,NULL); assert(thread);
#else
        pthread_t thread; assert(pthread_create(&thread,NULL,idle_thread,&state) == 0);
#endif
        unsigned attempts = 0;
        while (!atomic_load_explicit(&fixture.idle_issued,memory_order_acquire)) {
            assert(++attempts < 1000);
#ifdef _WIN32
            Sleep(1);
#else
            const struct timespec pause = {.tv_nsec = 1000000}; (void)nanosleep(&pause,NULL);
#endif
        }
        if (scenario == 0) {
            unsigned before = fixture.submissions;
            assert(submit(queues[0],0,NULL,NULL) == VK_ERROR_INITIALIZATION_FAILED);
            assert(idle(queues[0]) == VK_ERROR_DEVICE_LOST);
            assert(device_idle(device) == VK_ERROR_DEVICE_LOST);
            destroy_device(device,NULL);
            assert(fixture.submissions == before);
            assert(submit(queues[1],0,NULL,NULL) == VK_SUCCESS);
        } else {
            /* Defensive stale-namespace cleanup after the fake peer is retired. */
            venus_icd_abandon();
            if (scenario == 2) assert(venus_icd_bind(exchange,&replacement) == RingOk);
        }
#ifdef _WIN32
        assert(WaitForSingleObject(thread,2000) == WAIT_OBJECT_0); assert(CloseHandle(thread));
#else
        assert(pthread_join(thread,NULL) == 0);
#endif
        if (scenario == 0) {
            assert(state.result == VK_SUCCESS && idle(queues[1]) == VK_SUCCESS);
            destroy_device(device,NULL); destroy(instance);
        } else {
            assert(state.result == VK_ERROR_DEVICE_LOST);
        }
        assert(venus_icd_unbind() == RingOk);
    }
}

static void idle_failures(void) {
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        VkPhysicalDevice physical[2];
        uint32_t count = 2;
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
                   instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) ==
               VK_SUCCESS);
        float priority = 0.5f;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                   .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &queue_info};
        VkDevice device = NULL;
        assert(((PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice"))(
                   physical[0], &info, NULL, &device) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkQueueWaitIdle queue_idle = (PFN_vkQueueWaitIdle)lookup(device, "vkQueueWaitIdle");
        PFN_vkDeviceWaitIdle device_idle = (PFN_vkDeviceWaitIdle)lookup(device, "vkDeviceWaitIdle");
        VkQueue queue = NULL;
        ((PFN_vkGetDeviceQueue)lookup(device, "vkGetDeviceQueue"))(device, 0, 0, &queue);
        assert(queue);
        if (scenario < 3)
            fixture.gpu_corrupt = scenario + 1;
        else if (scenario == 3)
            fixture.gpu_failure = RingClosed;
        else if (scenario == 4)
            fixture.gpu_pending = UINT32_MAX;
        else if (scenario == 5)
            fixture.gpu_issue_pending = UINT32_MAX;
        else {
            assert(queue_idle(queue) == VK_SUCCESS);
            memset(fixture.gpu_issued, 0,
                   sizeof(fixture.gpu_issued)); /* Replay accepted identity. */
        }
        assert((scenario % 2 ? device_idle(device) : queue_idle(queue)) == VK_ERROR_DEVICE_LOST);
        assert(queue_idle(queue) == VK_ERROR_DEVICE_LOST);
        assert(device_idle(device) == VK_ERROR_DEVICE_LOST);
        assert(venus_icd_unbind() == RingAgain);
        venus_icd_abandon(); /* Fake backend has no external GPU resources. */
        assert(venus_icd_unbind() == RingOk);
    }
}
static void device_failures(void) {
    const uint32_t Commands[] = {11, 12, 155};
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        VkPhysicalDevice physical[2];
        uint32_t count = 2;
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices");
        assert(enumerate(instance, &count, physical) == VK_SUCCESS);
        PFN_vkCreateDevice device_create =
            (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        PFN_vkGetDeviceProcAddr device_proc =
            (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        float priority = 0.5f;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                   .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &queue_info};
        VkDevice device = NULL;
        assert(device_create(physical[0], &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(device_create(NULL, &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
        assert(device_create((VkPhysicalDevice)(uintptr_t)1, &info, NULL, &device) ==
               VK_ERROR_INITIALIZATION_FAILED);
        VkBaseInStructure chain = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
        info.pNext = &chain;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT);
        chain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        chain.pNext = &chain;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT);
        info.pNext = NULL;
        info.enabledLayerCount = 1;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_LAYER_NOT_PRESENT);
        info.enabledLayerCount = 0;
        info.enabledExtensionCount = 1;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT);
        info.enabledExtensionCount = 0;
        queue_info.queueCount = 0;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
        queue_info.queueCount = 1;
        fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.create_result = VK_SUCCESS;
        if (scenario >= 6) {
            fixture.create_result = scenario == 6 ? VK_ERROR_DEVICE_LOST : VK_NOT_READY;
            assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_DEVICE_LOST && !device);
            fixture.create_result = VK_SUCCESS;
            unsigned before = fixture.submissions;
            assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_DEVICE_LOST && !device);
            assert(fixture.submissions == before && venus_icd_unbind() == RingAgain);
            venus_icd_abandon(); continue;
        }
        const uint32_t command = Commands[scenario % 3];
        if (command != 11)
            assert(device_create(physical[0], &info, NULL, &device) == VK_SUCCESS);
        if (scenario < 3)
            fixture.corrupt_command = command;
        else
            fixture.fail_command = command;
        if (command == 11) {
            assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_DEVICE_LOST);
        } else if (command == 12) {
            ((PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice"))(device, NULL);
        } else {
            VkQueue queue = NULL;
            ((PFN_vkGetDeviceQueue)device_proc(device, "vkGetDeviceQueue"))(device, 0, 0, &queue);
            assert(!queue);
        }
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_DEVICE_LOST);
        assert(venus_icd_unbind() == RingAgain);
        venus_icd_abandon();
    }
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkInstance instance = create();
    VkPhysicalDevice physical[2];
    uint32_t count = 2;
    assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
               instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
    PFN_vkCreateDevice device_create =
        (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr device_proc =
        (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
    float priorities[16];
    for (unsigned index = 0; index < 16; index++)
        priorities[index] = 0.5f;
    VkDeviceQueueCreateInfo queues[16];
    for (unsigned index = 0; index < 16; index++)
        queues[index] =
            (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = index,
                                      .queueCount = 1,
                                      .pQueuePriorities = priorities};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = queues};
    VkDevice device;
    VkDevice devices[16];
    for (unsigned index = 0; index < 16; index++)
        assert(device_create(physical[0], &info, NULL, &devices[index]) == VK_SUCCESS);
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_OUT_OF_HOST_MEMORY);
    for (unsigned index = 0; index < 16; index++)
        ((PFN_vkDestroyDevice)device_proc(devices[index], "vkDestroyDevice"))(devices[index], NULL);
    info.queueCreateInfoCount = 0;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.queueCreateInfoCount = 17;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.queueCreateInfoCount = 1;
    info.sType = 0;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.flags = 1;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.flags = 0;
    info.pQueueCreateInfos = NULL;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.pQueueCreateInfos = queues;
    for (unsigned invalid = 0; invalid < 5; invalid++) {
        queues[0].queueCount = 1;
        queues[0].flags = 0;
        queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[0].pQueuePriorities = priorities;
        queues[0].pNext = NULL;
        if (invalid == 0)
            queues[0].sType = 0;
        if (invalid == 1)
            queues[0].flags = 1;
        if (invalid == 2)
            queues[0].queueCount = 17;
        if (invalid == 3)
            queues[0].pQueuePriorities = NULL;
        if (invalid == 4)
            queues[0].pNext = &info;
        assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    }
    queues[0].pNext = NULL;
    queues[0].pQueuePriorities = priorities;
    priorities[0] = -1;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    priorities[0] = 2;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    priorities[0] = 0.5f;
    info.queueCreateInfoCount = 2;
    queues[1].queueFamilyIndex = 0;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    queues[1].queueFamilyIndex = 1;
    info.queueCreateInfoCount = 16;
    for (unsigned index = 0; index < 16; index++)
        queues[index].queueCount = 16;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    info.queueCreateInfoCount = 1;
    VkPhysicalDeviceFeatures features = {.robustBufferAccess = 2};
    info.pEnabledFeatures = &features;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_INITIALIZATION_FAILED);
    features.robustBufferAccess = 1;
    for (unsigned index = 0; index < 4; index++) {
        queues[0].queueCount = index == 3 ? 15 : 16;
        assert(device_create(physical[0], &info, NULL, &devices[index]) == VK_SUCCESS);
    }
    queues[0].queueCount = 1;
    unsigned before = fixture.submissions;
    assert(device_create(physical[0], &info, NULL, &device) == VK_ERROR_OUT_OF_HOST_MEMORY && !device);
    assert(fixture.submissions == before);
    for (unsigned index = 0; index < 4; index++) {
        PFN_vkGetDeviceQueue get_queue =
            (PFN_vkGetDeviceQueue)device_proc(devices[index], "vkGetDeviceQueue");
        for (unsigned queue_index = 0; queue_index < 16; queue_index++) {
            VkQueue queue = NULL;
            get_queue(devices[index], 0, queue_index, &queue);
            assert((queue != NULL) == (index * 16 + queue_index < 63));
        }
    }
    for (unsigned index = 0; index < 4; index++)
        ((PFN_vkDestroyDevice)device_proc(devices[index], "vkDestroyDevice"))(devices[index], NULL);
    info.queueCreateInfoCount = 4;
    for (unsigned index = 0; index < 4; index++) queues[index].queueCount = 16;
    before = fixture.submissions;
    assert(device_create(physical[0],&info,NULL,&device) == VK_ERROR_OUT_OF_HOST_MEMORY && !device);
    assert(fixture.submissions == before);
    info.queueCreateInfoCount = 1; queues[0].queueCount = 1;
    assert(device_create(physical[0],&info,NULL,&device) == VK_SUCCESS);
    assert(((PFN_vkDeviceWaitIdle)device_proc(device,"vkDeviceWaitIdle"))(device) == VK_SUCCESS);
    ((PFN_vkDestroyDevice)device_proc(device,"vkDestroyDevice"))(device,NULL);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
}
/** @brief Reject malformed host limits before descriptor arithmetic, preserving sticky loss. */
static void descriptor_limits_contract(void) {
    for (unsigned mode = 2; mode <= 7; mode++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2; VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
        const VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &info;
        VkDevice device;
        assert(((PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice"))(physical[0], &info, NULL, &device) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkUpdateDescriptorSets update = (PFN_vkUpdateDescriptorSets)lookup(device, "vkUpdateDescriptorSets");
        fixture.descriptor_properties = mode;
        if (mode == 6) fixture.corrupt_command = 6;
        if (mode == 7) fixture.fail_command = 6;
        const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        update(device, 1, &write, 0, NULL);
        assert(fixture.command == 6);
        assert(((PFN_vkDeviceWaitIdle)lookup(device, "vkDeviceWaitIdle"))(device) == VK_ERROR_DEVICE_LOST);
        unsigned before = fixture.submissions;
        update(device, 1, &write, 0, NULL);
        assert(fixture.submissions == before);
        /* Fake receiver is retired here; uncertain frontend ownership clears only at abandon. */
        venus_icd_abandon();
        assert(venus_icd_unbind() == RingOk);
    }
}

/** @brief Validate native buffer update/copy packets and actual host bounds without descriptor retention. */
static void descriptor_update_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture,
                                      VkDescriptorSet source_set, VkDescriptorSet destination_set) {
    PFN_vkUpdateDescriptorSets update = (PFN_vkUpdateDescriptorSets)lookup(device, "vkUpdateDescriptorSets");
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
    PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
    PFN_vkFreeMemory release = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
    PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
    assert(update);
    fixture->descriptor_properties = 1;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    fixture->buffer_info = &buffer_info;
    VkBuffer buffers[4];
    for (unsigned index = 0; index < 3; index++) assert(create_buffer(device, &buffer_info, NULL, &buffers[index]) == VK_SUCCESS);
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    assert(create_buffer(device, &buffer_info, NULL, &buffers[3]) == VK_SUCCESS);
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536};
    fixture->memory_info = &memory_info;
    VkDeviceMemory memory;
    assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS);
    assert(bind(device, buffers[0], memory, 0) == VK_SUCCESS);
    assert(bind(device, buffers[1], memory, 16384) == VK_SUCCESS);
    assert(bind(device, buffers[3], memory, 32768) == VK_SUCCESS);
    VkDescriptorBufferInfo infos[2] = {{buffers[0], 64, 128}, {buffers[1], 128, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = source_set,
        .dstBinding = 3, .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = infos};
    VkCopyDescriptorSet copy = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = source_set,
        .srcBinding = 3, .dstSet = destination_set, .dstBinding = 3, .descriptorCount = 2};
    fixture->descriptor_writes = &write; fixture->descriptor_write_count = 1;
    fixture->descriptor_copies = &copy; fixture->descriptor_copy_count = 1;
    unsigned before = fixture->submissions;
    update(NULL, 1, &write, 0, NULL); update((VkDevice)(uintptr_t)1, 1, &write, 0, NULL);
    update(device, 0, NULL, 0, NULL); update(device, 65, (void *)(uintptr_t)1, 0, NULL);
    update(device, 0, NULL, 65, (void *)(uintptr_t)1); update(device, 1, NULL, 0, NULL); update(device, 0, NULL, 1, NULL);
    assert(fixture->submissions == before);
    update(device, 1, &write, 1, &copy); /* One properties query, then exact update packet. */
    assert(fixture->submissions == before + 2 && fixture->command == 79);
    before = fixture->submissions;
    fixture->descriptor_copy_count = 0;
    const VkWriteDescriptorSet original = write;
    write.sType = 0; update(device, 1, &write, 0, NULL); write = original;
    write.pNext = (void *)(uintptr_t)1; update(device, 1, &write, 0, NULL); write = original;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; update(device, 1, &write, 0, NULL); write = original;
    write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; update(device, 1, &write, 0, NULL); write = original;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; update(device, 1, &write, 0, NULL); write = original;
    write.descriptorCount = 0; update(device, 1, &write, 0, NULL); write = original;
    write.descriptorCount = 65; update(device, 1, &write, 0, NULL); write = original;
    write.pBufferInfo = NULL; update(device, 1, &write, 0, NULL); write = original;

    write.dstSet = NULL; update(device, 1, &write, 0, NULL); write = original;
    write.dstSet = (VkDescriptorSet)(uintptr_t)1; update(device, 1, &write, 0, NULL); write = original;
    write.dstBinding = 9; update(device, 1, &write, 0, NULL); write = original;
    write.dstArrayElement = 1; update(device, 1, &write, 0, NULL); write = original;
    const VkDescriptorBufferInfo saved = infos[0];
    infos[0].buffer = NULL; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].buffer = (VkBuffer)(uintptr_t)1; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].buffer = buffers[2]; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].buffer = buffers[3]; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].offset = 1; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].offset = 4096; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].range = 0; update(device, 1, &write, 0, NULL); infos[0] = saved;
    infos[0].range = 4096; update(device, 1, &write, 0, NULL); infos[0] = saved;
    assert(fixture->submissions == before);
    /* Ignored native arrays can carry arbitrary tokens; canonical wire emits absent arrays. */
    write.pImageInfo = (void *)(uintptr_t)1; write.pTexelBufferView = (void *)(uintptr_t)1;
    update(device, 1, &write, 0, NULL); write = original;
    assert(fixture->submissions == before + 1); before = fixture->submissions;
    fixture->descriptor_write_count = 0; fixture->descriptor_copy_count = 1;
    const VkCopyDescriptorSet original_copy = copy;
    copy.sType = 0; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.pNext = (void *)(uintptr_t)1; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.descriptorCount = 0; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.descriptorCount = 65; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.srcSet = NULL; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.dstSet = NULL; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.srcBinding = 9; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.dstBinding = 9; update(device, 0, NULL, 1, &copy); copy = original_copy;
    copy.dstSet = source_set; update(device, 0, NULL, 1, &copy); copy = original_copy;
    assert(fixture->submissions == before);
    update(device, 0, NULL, 1, &copy); assert(fixture->submissions == before + 1);
    /* Codec aggregate limit is enforced for otherwise-valid repeated updates/copies. */
    VkWriteDescriptorSet many_writes[64]; VkCopyDescriptorSet many_copies[64];
    for (unsigned index = 0; index < 64; index++) { many_writes[index] = write; many_copies[index] = copy; }
    before = fixture->submissions;
    update(device, 64, many_writes, 64, many_copies);
    assert(fixture->submissions == before);
    /* Descriptors do not artificially retain an application buffer after GPU retirement. */
    destroy_buffer(device, buffers[0], NULL);
    before = fixture->submissions;
    update(device, 1, &write, 0, NULL);
    assert(fixture->submissions == before);
    update(device, 0, NULL, 1, &copy); /* Copying a destroyed resource reference legally makes destination undefined. */
    assert(fixture->submissions == before + 1);
    for (unsigned index = 1; index < 4; index++) destroy_buffer(device, buffers[index], NULL);
    release(device, memory, NULL);
    fixture->descriptor_properties = 0; fixture->buffer_info = NULL; fixture->memory_info = NULL;
    fixture->descriptor_writes = NULL; fixture->descriptor_copies = NULL;
    fixture->descriptor_write_count = 0; fixture->descriptor_copy_count = 0;
}

/** @brief Verify transactional set publication, pool quota refunds and implicit ownership retirement. */
static void descriptor_lifecycle_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture) {
    PFN_vkCreateDescriptorSetLayout create_layout = (PFN_vkCreateDescriptorSetLayout)lookup(device, "vkCreateDescriptorSetLayout");
    PFN_vkDestroyDescriptorSetLayout destroy_layout = (PFN_vkDestroyDescriptorSetLayout)lookup(device, "vkDestroyDescriptorSetLayout");
    PFN_vkCreateDescriptorPool create_pool = (PFN_vkCreateDescriptorPool)lookup(device, "vkCreateDescriptorPool");
    PFN_vkDestroyDescriptorPool destroy_pool = (PFN_vkDestroyDescriptorPool)lookup(device, "vkDestroyDescriptorPool");
    PFN_vkResetDescriptorPool reset_pool = (PFN_vkResetDescriptorPool)lookup(device, "vkResetDescriptorPool");
    PFN_vkAllocateDescriptorSets allocate = (PFN_vkAllocateDescriptorSets)lookup(device, "vkAllocateDescriptorSets");
    PFN_vkFreeDescriptorSets free_sets = (PFN_vkFreeDescriptorSets)lookup(device, "vkFreeDescriptorSets");
    assert(create_pool && destroy_pool && reset_pool && allocate && free_sets);
    VkDescriptorSetLayoutBinding binding = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    fixture->descriptor_layout_info = &layout_info;
    VkDescriptorSetLayout layout;
    assert(create_layout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 128};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &size};
    fixture->descriptor_pool_info = &pool_info;
    VkDescriptorPool pools[3], pool = NULL;
    unsigned before = fixture->submissions;
    assert(create_pool(NULL, &pool_info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED && !pool);
    assert(create_pool(device, NULL, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED && !pool);
    assert(create_pool(device, &pool_info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_pool((VkDevice)(uintptr_t)1, &pool_info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED && !pool);
    pool_info.flags = 2;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED && !pool);
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !pool);
    fixture->create_result = VK_SUCCESS;
    for (unsigned index = 0; index < 3; index++) assert(create_pool(device, &pool_info, NULL, &pools[index]) == VK_SUCCESS);
    VkDescriptorSetLayout layouts[64];
    for (unsigned index = 0; index < 64; index++) layouts[index] = layout;
    VkDescriptorSetAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pools[0], .descriptorSetCount = 64, .pSetLayouts = layouts};
    fixture->descriptor_allocate_info = &info;
    VkDescriptorSet sets[3][64] = {{0}};
    before = fixture->submissions;
    assert(allocate(NULL, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED && !sets[0][0]);
    assert(allocate(device, NULL, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(allocate(device, &info, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    info.descriptorSetCount = 0;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    info.descriptorSetCount = 65;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_OUT_OF_HOST_MEMORY);
    info.descriptorSetCount = 64;
    info.sType = 0;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED && !sets[0][63]);
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.pNext = (void *)(uintptr_t)1;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    info.pNext = NULL; info.descriptorPool = NULL;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    info.descriptorPool = (VkDescriptorPool)(uintptr_t)1;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    info.descriptorPool = pools[0]; info.pSetLayouts = NULL;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    info.pSetLayouts = layouts; layouts[0] = NULL;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    layouts[0] = (VkDescriptorSetLayout)(uintptr_t)1;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    layouts[0] = layout;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !sets[0][63]);
    fixture->create_result = VK_SUCCESS;
    assert(allocate(device, &info, sets[0]) == VK_SUCCESS);
    before = fixture->submissions;
    assert(allocate(device, &info, sets[1]) == VK_ERROR_OUT_OF_HOST_MEMORY && !sets[1][0]);
    assert(fixture->submissions == before);
    info.descriptorPool = pools[1];
    assert(allocate(device, &info, sets[1]) == VK_SUCCESS);
    info.descriptorPool = pools[2];
    before = fixture->submissions;
    assert(allocate(device, &info, sets[2]) == VK_ERROR_OUT_OF_HOST_MEMORY && !sets[2][63]);
    assert(fixture->submissions == before);
    assert(free_sets(device, pools[0], 0, NULL) == VK_SUCCESS);
    assert(free_sets(NULL, pools[0], 1, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(free_sets(device, NULL, 1, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(free_sets(device, pools[0], 65, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(free_sets(device, pools[0], 1, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    VkDescriptorSet duplicate[2] = {sets[0][0], sets[0][0]};
    assert(free_sets(device, pools[0], 2, duplicate) == VK_ERROR_INITIALIZATION_FAILED);
    assert(free_sets(device, pools[1], 1, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    const VkDescriptorSet null_set = NULL;
    assert(free_sets(device, pools[0], 1, &null_set) == VK_ERROR_INITIALIZATION_FAILED);
    fixture->command_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(free_sets(device, pools[0], 64, sets[0]) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    fixture->command_result = VK_SUCCESS;
    assert(free_sets(device, pools[0], 64, sets[0]) == VK_SUCCESS);
    assert(free_sets(device, pools[0], 1, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(allocate(device, &info, sets[2]) == VK_SUCCESS);
    descriptor_update_contract(device, lookup, fixture, sets[2][0], sets[2][1]);
    /* Original layout may retire while sets retain copied definitions. */
    destroy_layout(device, layout, NULL);
    assert(reset_pool(NULL, pools[1], 0) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset_pool(device, NULL, 0) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset_pool(device, pools[1], 1) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset_pool((VkDevice)(uintptr_t)1, pools[1], 0) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset_pool(device, (VkDescriptorPool)(uintptr_t)1, 0) == VK_ERROR_INITIALIZATION_FAILED);
    fixture->pool_reset_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(reset_pool(device, pools[1], 0) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    fixture->pool_reset_result = VK_SUCCESS;
    assert(reset_pool(device, pools[1], 0) == VK_SUCCESS);
    assert(free_sets(device, pools[1], 1, sets[1]) == VK_ERROR_INITIALIZATION_FAILED);
    destroy_pool(device, pools[2], NULL); /* Native destruction implicitly retires64 sets. */
    destroy_pool(device, pools[1], NULL); destroy_pool(device, pools[0], NULL);
    before = fixture->submissions;
    destroy_pool(NULL, pools[0], NULL); destroy_pool(device, NULL, NULL); destroy_pool(device, pools[0], NULL);
    assert(fixture->submissions == before);
    /* Per-type quota checked separately from set quota; reset-only pool disallows free. */
    pool_info.flags = 0; pool_info.maxSets = 2; size.descriptorCount = 1;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    assert(create_layout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    layouts[0] = layout; info.descriptorPool = pool; info.descriptorSetCount = 1;
    before = fixture->submissions;
    assert(allocate(device, &info, sets[0]) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(fixture->submissions == before);
    destroy_pool(device, pool, NULL);
    size.descriptorCount = 2;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    info.descriptorPool = pool;
    assert(allocate(device, &info, sets[0]) == VK_SUCCESS);
    assert(free_sets(device, pool, 1, sets[0]) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset_pool(device, pool, 0) == VK_SUCCESS);
    assert(allocate(device, &info, sets[0]) == VK_SUCCESS);
    destroy_pool(device, pool, NULL); destroy_layout(device, layout, NULL);
    fixture->descriptor_layout_info = NULL; fixture->descriptor_pool_info = NULL; fixture->descriptor_allocate_info = NULL;
}

/** @brief Verify acknowledged compute recording and descriptor-only pending buffer retention. */
static void compute_recording_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture,
    VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout descriptor_layout) {
#define LoadCompute(type, variable, name) type variable = (type)lookup(device, name); assert(variable)
    LoadCompute(PFN_vkCreateDescriptorPool, create_descriptor_pool, "vkCreateDescriptorPool");
    LoadCompute(PFN_vkDestroyDescriptorPool, destroy_descriptor_pool, "vkDestroyDescriptorPool");
    LoadCompute(PFN_vkAllocateDescriptorSets, allocate_sets, "vkAllocateDescriptorSets");
    LoadCompute(PFN_vkFreeDescriptorSets, free_sets, "vkFreeDescriptorSets");
    LoadCompute(PFN_vkUpdateDescriptorSets, update_sets, "vkUpdateDescriptorSets");
    LoadCompute(PFN_vkCreateBuffer, create_buffer, "vkCreateBuffer");
    LoadCompute(PFN_vkDestroyBuffer, destroy_buffer, "vkDestroyBuffer");
    LoadCompute(PFN_vkAllocateMemory, allocate_memory, "vkAllocateMemory");
    LoadCompute(PFN_vkFreeMemory, free_memory, "vkFreeMemory");
    LoadCompute(PFN_vkBindBufferMemory, bind_memory, "vkBindBufferMemory");
    LoadCompute(PFN_vkCreateCommandPool, create_pool, "vkCreateCommandPool");
    LoadCompute(PFN_vkDestroyCommandPool, destroy_pool, "vkDestroyCommandPool");
    LoadCompute(PFN_vkAllocateCommandBuffers, allocate_commands, "vkAllocateCommandBuffers");
    LoadCompute(PFN_vkBeginCommandBuffer, begin, "vkBeginCommandBuffer");
    LoadCompute(PFN_vkEndCommandBuffer, end, "vkEndCommandBuffer");
    LoadCompute(PFN_vkResetCommandBuffer, reset, "vkResetCommandBuffer");
    LoadCompute(PFN_vkCmdBindPipeline, bind_pipeline, "vkCmdBindPipeline");
    LoadCompute(PFN_vkCmdBindDescriptorSets, bind_sets, "vkCmdBindDescriptorSets");
    LoadCompute(PFN_vkCmdDispatch, dispatch, "vkCmdDispatch");
    LoadCompute(PFN_vkCmdPushConstants, push, "vkCmdPushConstants");
    LoadCompute(PFN_vkQueueSubmit, submit, "vkQueueSubmit");
    LoadCompute(PFN_vkQueueWaitIdle, idle, "vkQueueWaitIdle");
    LoadCompute(PFN_vkGetDeviceQueue, get_queue, "vkGetDeviceQueue");
#undef LoadCompute
    fixture->descriptor_properties = 1;
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,.maxSets=2,.poolSizeCount=1,.pPoolSizes=&size};
    fixture->descriptor_pool_info=&descriptor_pool_info; VkDescriptorPool descriptor_pool;
    assert(create_descriptor_pool(device,&descriptor_pool_info,NULL,&descriptor_pool)==VK_SUCCESS);
    VkDescriptorSetLayout set_layouts[2]={descriptor_layout,descriptor_layout};
    VkDescriptorSetAllocateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,
        .descriptorSetCount=2,.pSetLayouts=set_layouts};
    fixture->descriptor_allocate_info=&set_info;VkDescriptorSet sets[2];
    assert(allocate_sets(device,&set_info,sets)==VK_SUCCESS);VkDescriptorSet set=sets[0];
    VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    fixture->buffer_info=&buffer_info;VkBuffer buffer;assert(create_buffer(device,&buffer_info,NULL,&buffer)==VK_SUCCESS);
    VkMemoryAllocateInfo memory_info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=16384};
    fixture->memory_info=&memory_info;VkDeviceMemory memory;assert(allocate_memory(device,&memory_info,NULL,&memory)==VK_SUCCESS);
    assert(bind_memory(device,buffer,memory,0)==VK_SUCCESS);
    VkDescriptorBufferInfo buffer_descriptor={buffer,0,256};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_descriptor};
    fixture->descriptor_writes=&write;fixture->descriptor_write_count=1;fixture->descriptor_copy_count=0;
    update_sets(device,1,&write,0,NULL);assert(fixture->command==79);
    VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    fixture->pool_info=&pool_info;VkCommandPool pool;assert(create_pool(device,&pool_info,NULL,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.commandBufferCount=1};
    fixture->command_allocate=&command_info;VkCommandBuffer command;assert(allocate_commands(device,&command_info,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};fixture->command_begin=&begin_info;
    VkQueue queue;get_queue(device,0,0,&queue);assert(queue);
    VkSubmitInfo submit_info={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    unsigned ignored_before=fixture->submissions;
    VkCommandBuffer ignored_commands[3]={NULL,(VkCommandBuffer)(uintptr_t)1,command};
    for(unsigned index=0;index<3;++index){
        bind_pipeline(ignored_commands[index],1,pipeline);
        bind_sets(ignored_commands[index],1,layout,0,1,&set,0,NULL);
        uint32_t ignored_value=1;push(ignored_commands[index],layout,32,0,4,&ignored_value);
        dispatch(ignored_commands[index],1,1,1);
    }
    assert(fixture->submissions==ignored_before);
    for(unsigned invalid=0;invalid<28;++invalid){
        assert(begin(command,&begin_info)==VK_SUCCESS);unsigned before=fixture->submissions;
        if(invalid==0)bind_pipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        if(invalid==1)bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,NULL);
        if(invalid==2)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,16,1,(void*)(uintptr_t)1,0,NULL);
        if(invalid==3)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,0,(void*)(uintptr_t)1,0,NULL);
        if(invalid==4)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,1,(void*)(uintptr_t)1);
        if(invalid==5)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,NULL,0,NULL);
        if(invalid==6)dispatch(command,1,1,1);
        if(invalid==7)push(command,layout,32,1,4,(void*)(uintptr_t)1);
        if(invalid==8)push(command,layout,32,0,132,(void*)(uintptr_t)1);
        VkDescriptorSet stale_set=(VkDescriptorSet)(uintptr_t)1;
        if(invalid==9)bind_sets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
        if(invalid==10)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,NULL,0,1,&set,0,NULL);
        if(invalid==11)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,(VkPipelineLayout)(uintptr_t)1,0,1,&set,0,NULL);
        if(invalid==12)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,1,1,&set,0,NULL);
        if(invalid==13)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,17,(void*)(uintptr_t)1,0,NULL);
        if(invalid==14)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&stale_set,0,NULL);
        if(invalid==15)push(command,NULL,32,0,4,(void*)(uintptr_t)1);
        if(invalid==16)push(command,(VkPipelineLayout)(uintptr_t)1,32,0,4,(void*)(uintptr_t)1);
        if(invalid==17)push(command,layout,32,0,4,NULL);
        if(invalid==18)push(command,layout,0,0,4,(void*)(uintptr_t)1);
        if(invalid==19)push(command,layout,64,0,4,(void*)(uintptr_t)1);
        if(invalid==20)push(command,layout,32,0,0,(void*)(uintptr_t)1);
        if(invalid==21)push(command,layout,32,0,3,(void*)(uintptr_t)1);
        if(invalid==22)push(command,layout,32,132,4,(void*)(uintptr_t)1);
        if(invalid==23)push(command,layout,1,0,4,(void*)(uintptr_t)1);
        if(invalid==24)push(command,layout,32,4,4,(void*)(uintptr_t)1);
        if(invalid==25)bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,(VkPipeline)(uintptr_t)1);
        if(invalid==26)bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[1],0,NULL);
        if(invalid==26){bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);before=fixture->submissions;dispatch(command,1,1,1);}
        if(invalid==27){bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);before=fixture->submissions;dispatch(command,9,1,1);}
        assert(fixture->submissions==before);assert(end(command)==VK_ERROR_INITIALIZATION_FAILED);
        assert(reset(command,0)==VK_SUCCESS);
    }
    assert(begin(command,&begin_info)==VK_SUCCESS);
    /* Ordinary Vulkan ordering permits descriptors and push writes before pipeline binding. */
    /* The first bound set is never consumed and remains undefined. */
    bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[1],0,NULL);
    bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    uint32_t value=17;push(command,layout,32,0,sizeof(value),&value);assert(fixture->command==132);
    bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);dispatch(command,8,1,1);assert(fixture->command==110);
    assert(end(command)==VK_SUCCESS);
    fixture->submit_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(submit(queue,1,&submit_info,NULL)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    fixture->submit_result=VK_SUCCESS;
    assert(submit(queue,1,&submit_info,NULL)==VK_SUCCESS);
    unsigned before=fixture->submissions;
    destroy_buffer(device,buffer,NULL);free_memory(device,memory,NULL);update_sets(device,1,&write,0,NULL);
    assert(free_sets(device,descriptor_pool,1,&set)==VK_ERROR_INITIALIZATION_FAILED);
    destroy_descriptor_pool(device,descriptor_pool,NULL);
    assert(fixture->submissions==before);
    assert(idle(queue)==VK_SUCCESS);
    /* A successful core descriptor update invalidates the completed recorded binding. */
    update_sets(device,1,&write,0,NULL);
    before=fixture->submissions;assert(submit(queue,1,&submit_info,NULL)==VK_ERROR_INITIALIZATION_FAILED);assert(fixture->submissions==before);
    assert(reset(command,0)==VK_SUCCESS);assert(begin(command,&begin_info)==VK_SUCCESS);
    bind_sets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    bind_pipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);dispatch(command,1,1,1);assert(end(command)==VK_SUCCESS);
    /* Destroying an actually consumed buffer invalidates an executable command. */
    destroy_buffer(device,buffer,NULL);before=fixture->submissions;
    assert(submit(queue,1,&submit_info,NULL)==VK_ERROR_INITIALIZATION_FAILED);assert(fixture->submissions==before);
    destroy_pool(device,pool,NULL);destroy_descriptor_pool(device,descriptor_pool,NULL);free_memory(device,memory,NULL);
    fixture->descriptor_pool_info=NULL;fixture->descriptor_allocate_info=NULL;fixture->descriptor_writes=NULL;fixture->descriptor_write_count=0;
    fixture->buffer_info=NULL;fixture->memory_info=NULL;fixture->pool_info=NULL;fixture->command_allocate=NULL;fixture->command_begin=NULL;
}

/** @brief Verify copied pipeline lifetime, native failures and fixed64-owner exhaustion. */
static void compute_pipeline_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture) {
    PFN_vkCreateShaderModule create_shader=(PFN_vkCreateShaderModule)lookup(device,"vkCreateShaderModule");
    PFN_vkDestroyShaderModule destroy_shader=(PFN_vkDestroyShaderModule)lookup(device,"vkDestroyShaderModule");
    PFN_vkCreatePipelineLayout create_layout=(PFN_vkCreatePipelineLayout)lookup(device,"vkCreatePipelineLayout");
    PFN_vkDestroyPipelineLayout destroy_layout=(PFN_vkDestroyPipelineLayout)lookup(device,"vkDestroyPipelineLayout");
    PFN_vkCreateComputePipelines create=(PFN_vkCreateComputePipelines)lookup(device,"vkCreateComputePipelines");
    PFN_vkDestroyPipeline destroy=(PFN_vkDestroyPipeline)lookup(device,"vkDestroyPipeline");
    assert(create&&destroy);
    VkShaderModuleCreateInfo shader_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(ComputeShader),.pCode=ComputeShader};
    fixture->shader_info=&shader_info;
    VkShaderModule shader; assert(create_shader(device,&shader_info,NULL,&shader)==VK_SUCCESS);
    PFN_vkCreateDescriptorSetLayout create_descriptor_layout=(PFN_vkCreateDescriptorSetLayout)lookup(device,"vkCreateDescriptorSetLayout");
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_layout=(PFN_vkDestroyDescriptorSetLayout)lookup(device,"vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT}};
    VkDescriptorSetLayoutCreateInfo descriptor_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    fixture->descriptor_layout_info=&descriptor_info;VkDescriptorSetLayout descriptor_layout;
    assert(create_descriptor_layout(device,&descriptor_info,NULL,&descriptor_layout)==VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&descriptor_layout};
    fixture->pipeline_layout_info=&layout_info;
    VkPipelineLayout layout; assert(create_layout(device,&layout_info,NULL,&layout)==VK_SUCCESS);
    VkPushConstantRange push_range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=4};
    /* Pipeline layout already exists; create a matching push-capable replacement. */
    destroy_layout(device,layout,NULL);layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&push_range;
    assert(create_layout(device,&layout_info,NULL,&layout)==VK_SUCCESS);
    VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=layout};
    fixture->compute_info=&info;
    VkPipeline pipeline=NULL,pipelines[64];unsigned before=fixture->submissions;
    assert(create(device,NULL,1,&info,NULL,NULL)==VK_ERROR_INITIALIZATION_FAILED);
    assert(create(NULL,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);
    assert(create((VkDevice)(uintptr_t)1,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);
    assert(create(device,NULL,1,NULL,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);
    assert(create(device,(VkPipelineCache)(uintptr_t)1,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);
    pipeline=(VkPipeline)(uintptr_t)42;
    assert(create(device,NULL,0,(void*)(uintptr_t)1,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&pipeline==(VkPipeline)(uintptr_t)42);
    assert(create(device,NULL,2,(void*)(uintptr_t)1,NULL,(void*)(uintptr_t)1)==VK_ERROR_INITIALIZATION_FAILED);
    info.stage.module=NULL;assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);info.stage.module=shader;
    info.layout=NULL;assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);info.layout=layout;
    info.flags=1;assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);info.flags=0;
    assert(fixture->submissions==before);
    fixture->create_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!pipeline);
    fixture->create_result=VK_SUCCESS;
    for(unsigned index=0;index<64;++index)assert(create(device,NULL,1,&info,NULL,&pipelines[index])==VK_SUCCESS);
    before=fixture->submissions;
    assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_OUT_OF_HOST_MEMORY&&!pipeline);
    assert(fixture->submissions==before);
    compute_recording_contract(device,lookup,fixture,pipelines[0],layout,descriptor_layout);
    destroy_descriptor_layout(device,descriptor_layout,NULL);
    destroy_shader(device,shader,NULL);destroy_layout(device,layout,NULL);
    before=fixture->submissions;
    assert(create(device,NULL,1,&info,NULL,&pipeline)==VK_ERROR_INITIALIZATION_FAILED&&!pipeline);
    destroy(NULL,pipelines[0],NULL);destroy((VkDevice)(uintptr_t)1,pipelines[0],NULL);destroy(device,NULL,NULL);destroy(device,(VkPipeline)(uintptr_t)1,NULL);
    assert(fixture->submissions==before);
    for(unsigned index=0;index<64;++index)destroy(device,pipelines[index],NULL);
    before=fixture->submissions;destroy(device,pipelines[0],NULL);assert(fixture->submissions==before);
    fixture->descriptor_layout_info=NULL;fixture->compute_info=NULL;fixture->shader_info=NULL;fixture->pipeline_layout_info=NULL;
}

/** @brief Prove copied layout ownership, native identity translation and fixed quota refund. */
static void layout_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture) {
    PFN_vkCreateDescriptorSetLayout create_layout = (PFN_vkCreateDescriptorSetLayout)lookup(device, "vkCreateDescriptorSetLayout");
    PFN_vkDestroyDescriptorSetLayout destroy_layout = (PFN_vkDestroyDescriptorSetLayout)lookup(device, "vkDestroyDescriptorSetLayout");
    PFN_vkCreatePipelineLayout create_pipeline = (PFN_vkCreatePipelineLayout)lookup(device, "vkCreatePipelineLayout");
    PFN_vkDestroyPipelineLayout destroy_pipeline = (PFN_vkDestroyPipelineLayout)lookup(device, "vkDestroyPipelineLayout");
    assert(create_layout && destroy_layout && create_pipeline && destroy_pipeline);
    VkDescriptorSetLayoutBinding binding = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    fixture->descriptor_layout_info = &info;
    VkDescriptorSetLayout layout = NULL;
    unsigned before = fixture->submissions;
    assert(create_layout(NULL, &info, NULL, &layout) == VK_ERROR_INITIALIZATION_FAILED && !layout);
    assert(create_layout(device, NULL, NULL, &layout) == VK_ERROR_INITIALIZATION_FAILED && !layout);
    assert(create_layout(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_layout((VkDevice)(uintptr_t)1, &info, NULL, &layout) == VK_ERROR_INITIALIZATION_FAILED && !layout);
    info.flags = 1;
    assert(create_layout(device, &info, NULL, &layout) == VK_ERROR_INITIALIZATION_FAILED && !layout);
    info.flags = 0;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_layout(device, &info, NULL, &layout) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !layout);
    fixture->create_result = VK_SUCCESS;
    VkDescriptorSetLayout layouts[32];
    for (unsigned index = 0; index < 32; index++) assert(create_layout(device, &info, NULL, &layouts[index]) == VK_SUCCESS);
    before = fixture->submissions;
    assert(create_layout(device, &info, NULL, &layout) == VK_ERROR_OUT_OF_HOST_MEMORY && !layout);
    assert(fixture->submissions == before);
    VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4};
    VkPipelineLayoutCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layouts[0], .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    fixture->pipeline_layout_info = &pipeline_info;
    VkPipelineLayout pipeline = NULL;
    assert(create_pipeline(NULL, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    assert(create_pipeline(device, NULL, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    assert(create_pipeline(device, &pipeline_info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_pipeline((VkDevice)(uintptr_t)1, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    pipeline_info.setLayoutCount = 17;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    pipeline_info.setLayoutCount = 1;
    const VkDescriptorSetLayout saved_layout = layouts[0];
    layouts[0] = NULL;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    layouts[0] = (VkDescriptorSetLayout)(uintptr_t)1;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    layouts[0] = saved_layout;
    before = fixture->submissions;
    range.offset = 128;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    range.offset = UINT32_MAX;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    range.offset = 0;
    pipeline_info.pPushConstantRanges = NULL;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    pipeline_info.pPushConstantRanges = &range;
    assert(fixture->submissions == before);
    range.stageFlags = 0;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !pipeline);
    fixture->create_result = VK_SUCCESS;
    VkPipelineLayout pipelines[32];
    for (unsigned index = 0; index < 32; index++) assert(create_pipeline(device, &pipeline_info, NULL, &pipelines[index]) == VK_SUCCESS);
    before = fixture->submissions;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_ERROR_OUT_OF_HOST_MEMORY && !pipeline);
    assert(fixture->submissions == before);
    /* Copied pipeline definitions outlive every original descriptor layout token. */
    for (unsigned index = 0; index < 32; index++) destroy_layout(device, layouts[index], NULL);
    before = fixture->submissions;
    destroy_layout(device, layouts[0], NULL); destroy_layout(NULL, layouts[0], NULL); destroy_layout(device, NULL, NULL);
    assert(fixture->submissions == before);
    for (unsigned index = 0; index < 32; index++) destroy_pipeline(device, pipelines[index], NULL);
    before = fixture->submissions;
    destroy_pipeline(device, pipelines[0], NULL); destroy_pipeline(NULL, pipelines[0], NULL); destroy_pipeline(device, NULL, NULL);
    assert(fixture->submissions == before);
    /* Full-profile reclaim after exact native acknowledgments. */
    assert(create_layout(device, &info, NULL, &layout) == VK_SUCCESS);
    pipeline_info.pSetLayouts = &layout;
    assert(create_pipeline(device, &pipeline_info, NULL, &pipeline) == VK_SUCCESS);
    destroy_layout(device, layout, NULL); destroy_pipeline(device, pipeline, NULL);
    fixture->descriptor_layout_info = NULL; fixture->pipeline_layout_info = NULL;
}

static void shader_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture) {
    PFN_vkCreateShaderModule create_shader = (PFN_vkCreateShaderModule)lookup(device, "vkCreateShaderModule");
    PFN_vkDestroyShaderModule destroy_shader = (PFN_vkDestroyShaderModule)lookup(device, "vkDestroyShaderModule");
    assert(create_shader && destroy_shader);
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(ComputeShader), .pCode = ComputeShader};
    fixture->shader_info = &info;
    VkShaderModule shader = NULL;
    unsigned before = fixture->submissions;
    assert(create_shader(device, NULL, NULL, &shader) == VK_ERROR_INITIALIZATION_FAILED && !shader);
    assert(create_shader(NULL, &info, NULL, &shader) == VK_ERROR_INITIALIZATION_FAILED && !shader);
    assert(create_shader(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_shader((VkDevice)(uintptr_t)1, &info, NULL, &shader) == VK_ERROR_INITIALIZATION_FAILED && !shader);
    info.flags = 1;
    assert(create_shader(device, &info, NULL, &shader) == VK_ERROR_INITIALIZATION_FAILED && !shader);
    info.flags = 0;
    info.codeSize = 8192;
    assert(create_shader(device, &info, NULL, &shader) == VK_ERROR_OUT_OF_HOST_MEMORY && !shader);
    info.codeSize = sizeof(ComputeShader);
    info.pCode = NULL;
    assert(create_shader(device, &info, NULL, &shader) == VK_ERROR_INITIALIZATION_FAILED && !shader);
    info.pCode = ComputeShader;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_shader(device, &info, NULL, &shader) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !shader);
    fixture->create_result = VK_SUCCESS;
    assert(create_shader(device, &info, NULL, &shader) == VK_SUCCESS && shader);
    destroy_shader(device, shader, NULL);
    before = fixture->submissions;
    destroy_shader(device, shader, NULL);
    destroy_shader(NULL, shader, NULL);
    destroy_shader(device, NULL, NULL);
    assert(fixture->submissions == before);
    fixture->shader_info = NULL;
}
/** @brief Compare canonical pass bytes and local/ordinary-host failure rollback.
 * @param[in] device Live borrowed guest device. @param[in] lookup Static device dispatcher.
 * @param[in,out] fixture Borrowed single-thread backend; pass-info pointer cleared on return.
 * @note Owned pass identity destroyed after each successful iteration; no heap allocation.
 */
static void render_pass_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup, fixture_t *fixture) {
    PFN_vkCreateRenderPass create_pass = (PFN_vkCreateRenderPass)lookup(device, "vkCreateRenderPass");
    PFN_vkDestroyRenderPass destroy_pass = (PFN_vkDestroyRenderPass)lookup(device, "vkDestroyRenderPass");
    assert(create_pass && destroy_pass);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    fixture->render_pass_info = &info;
    VkRenderPass pass = (VkRenderPass)(uintptr_t)42;
    unsigned before = fixture->submissions;
    assert(create_pass(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_pass(NULL, &info, NULL, &pass) == VK_ERROR_INITIALIZATION_FAILED && !pass);
    assert(create_pass((VkDevice)(uintptr_t)42, &info, NULL, &pass) == VK_ERROR_INITIALIZATION_FAILED && !pass);
    assert(create_pass(device, NULL, NULL, &pass) == VK_ERROR_INITIALIZATION_FAILED && !pass);
    info.flags = 1;
    assert(create_pass(device, &info, NULL, &pass) == VK_ERROR_INITIALIZATION_FAILED && !pass);
    info.flags = 0;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_pass(device, &info, NULL, &pass) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !pass);
    fixture->create_result = VK_SUCCESS;
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        attachment.format = iteration & 1 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        assert(create_pass(device, &info, NULL, &pass) == VK_SUCCESS && pass);
        before = fixture->submissions;
        destroy_pass(NULL, pass, NULL);
        destroy_pass(device, NULL, NULL);
        assert(fixture->submissions == before);
        destroy_pass(device, pass, NULL);
        assert(fixture->submissions == before + 1 && fixture->command == 83);
        destroy_pass(device, pass, NULL);
        assert(fixture->submissions == before + 1);
    }
    fixture->render_pass_info = NULL;
}

/** @brief Framebuffer frontend identity/bounds/lifecycle contract.
 * @param[in] device Live borrowed device; lookup immutable device resolver.
 * @param[in,out] fixture Sole-thread fake backend, borrows create infos only for calls.
 * @note All successfully created identities are explicitly retired; no heap storage.
 */
static void framebuffer_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup,
                                 fixture_t *fixture) {
    PFN_vkCreateRenderPass create_pass =
        (PFN_vkCreateRenderPass)lookup(device, "vkCreateRenderPass");
    PFN_vkDestroyRenderPass destroy_pass =
        (PFN_vkDestroyRenderPass)lookup(device, "vkDestroyRenderPass");
    PFN_vkCreateFramebuffer create_fb =
        (PFN_vkCreateFramebuffer)lookup(device, "vkCreateFramebuffer");
    PFN_vkDestroyFramebuffer destroy_fb =
        (PFN_vkDestroyFramebuffer)lookup(device, "vkDestroyFramebuffer");
    PFN_vkCreateImage create_image = (PFN_vkCreateImage)lookup(device, "vkCreateImage");
    PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)lookup(device, "vkDestroyImage");
    PFN_vkGetImageMemoryRequirements requirements =
        (PFN_vkGetImageMemoryRequirements)lookup(device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)lookup(device, "vkBindImageMemory");
    PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)lookup(device, "vkCreateImageView");
    PFN_vkDestroyImageView destroy_view =
        (PFN_vkDestroyImageView)lookup(device, "vkDestroyImageView");
    assert(create_fb && destroy_fb);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass};
    fixture->render_pass_info = &pass_info;
    VkRenderPass pass, other_pass;
    assert(create_pass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    assert(create_pass(device, &pass_info, NULL, &other_pass) == VK_SUCCESS);
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = {64, 64, 1},
                                    .mipLevels = 2,
                                    .arrayLayers = 2,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    fixture->image_info = &image_info;
    VkImage image;
    assert(create_image(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements memory_requirements = {0};
    requirements(device, image, &memory_requirements);
    assert(memory_requirements.size);
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .allocationSize = memory_requirements.size};
    fixture->memory_info = &memory_info;
    VkDeviceMemory memory;
    assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS);
    assert(bind(device, image, memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = image,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fixture->view_info = &view_info;
    VkImageView view;
    assert(create_view(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = pass,
                                    .attachmentCount = 1,
                                    .pAttachments = &view,
                                    .width = 64,
                                    .height = 64,
                                    .layers = 1};
    fixture->framebuffer_info = &info;
    VkFramebuffer framebuffer = (VkFramebuffer)(uintptr_t)42;
    unsigned before = fixture->submissions;
    assert(create_fb(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_fb(NULL, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(create_fb((VkDevice)(uintptr_t)1, &info, NULL, &framebuffer) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(create_fb(device, NULL, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.pNext = (void *)(uintptr_t)1;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.pNext = NULL;
    info.flags = 1;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.flags = 0;
    info.attachmentCount = 0;
    info.pAttachments = (void *)(uintptr_t)1;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.attachmentCount = 2;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.attachmentCount = 1;
    info.pAttachments = NULL;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.pAttachments = &view;
    info.renderPass = NULL;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.renderPass = other_pass;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    info.renderPass = pass;
    for (unsigned index = 0; index < 6; ++index) {
        info.width = index == 0 ? 0 : index == 1 ? 65 : 64;
        info.height = index == 2 ? 0 : index == 3 ? 65 : 64;
        info.layers = index == 4 ? 0 : index == 5 ? 2 : 1;
        assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
               !framebuffer);
    }
    info.width = 64;
    info.height = 64;
    info.layers = 1;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
           !framebuffer);
    fixture->create_result = VK_SUCCESS;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_SUCCESS && framebuffer);
    before = fixture->submissions;
    destroy_fb(NULL, framebuffer, NULL);
    destroy_fb(device, NULL, NULL);
    destroy_fb(device, (VkFramebuffer)(uintptr_t)1, NULL);
    assert(fixture->submissions == before);
    destroy_fb(device, framebuffer, NULL);
    assert(fixture->submissions == before + 1 && fixture->command == 81);
    destroy_fb(device, framebuffer, NULL);
    assert(fixture->submissions == before + 1);
    /* Bounds must use the selected mip, not the base image extent. */
    view_info.subresourceRange.baseMipLevel = 1;
    VkImageView mip_view;
    assert(create_view(device, &view_info, NULL, &mip_view) == VK_SUCCESS);
    info.pAttachments = &mip_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    info.width = 32;
    info.height = 32;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_SUCCESS);
    destroy_fb(device, framebuffer, NULL);
    destroy_view(device, mip_view, NULL);
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.components.r = VK_COMPONENT_SWIZZLE_B;
    VkImageView swizzle_view;
    assert(create_view(device, &view_info, NULL, &swizzle_view) == VK_SUCCESS);
    info.pAttachments = &swizzle_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, swizzle_view, NULL);
    /* Reject views spanning multiple native mip/layer attachments. */
    view_info.components = (VkComponentMapping){0};
    view_info.subresourceRange.levelCount = 2;
    VkImageView wide_view;
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, wide_view, NULL);
    view_info.subresourceRange.levelCount = 1;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.subresourceRange.layerCount = 2;
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, wide_view, NULL);
    /* Remaining counts resolve to exactly one mip/layer at the final subresource. */
    view_info.subresourceRange = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_REMAINING_MIP_LEVELS, 1, VK_REMAINING_ARRAY_LAYERS};
    view_info.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_SUCCESS);
    destroy_fb(device, framebuffer, NULL);
    destroy_view(device, wide_view, NULL);
    /* A stale attachment rejects independently of a live creating render pass. */
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    VkImageView absent_view = NULL;
    info.pAttachments = &absent_view;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    /* A bound image without COLOR_ATTACHMENT usage cannot back a framebuffer. */
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage transfer_image;
    assert(create_image(device, &image_info, NULL, &transfer_image) == VK_SUCCESS);
    assert(bind(device, transfer_image, memory, 0) == VK_SUCCESS);
    view_info.image = transfer_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.components = (VkComponentMapping){0};
    view_info.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, wide_view, NULL);
    destroy_image(device, transfer_image, NULL);
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    view_info.image = image;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_4_BIT;
    assert(create_image(device, &image_info, NULL, &transfer_image) == VK_SUCCESS);
    assert(bind(device, transfer_image, memory, 0) == VK_SUCCESS);
    view_info.image = transfer_image;
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, wide_view, NULL);
    destroy_image(device, transfer_image, NULL);
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.imageType = VK_IMAGE_TYPE_1D;
    image_info.extent.height = 1;
    assert(create_image(device, &image_info, NULL, &transfer_image) == VK_SUCCESS);
    assert(bind(device, transfer_image, memory, 0) == VK_SUCCESS);
    view_info.image = transfer_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_1D;
    assert(create_view(device, &view_info, NULL, &wide_view) == VK_SUCCESS);
    info.pAttachments = &wide_view;
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_ERROR_INITIALIZATION_FAILED &&
           !framebuffer);
    assert(fixture->submissions == before);
    destroy_view(device, wide_view, NULL);
    destroy_image(device, transfer_image, NULL);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.height = 64;
    image_info.mipLevels = 2;
    image_info.arrayLayers = 2;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.pAttachments = &view;
    info.width = 64;
    info.height = 64;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_SUCCESS);
    /* FB copies pass compatibility; quiescent view destruction is legal. */
    destroy_pass(device, pass, NULL);
    destroy_view(device, view, NULL);
    before = fixture->submissions;
    assert(create_fb(device, &info, NULL, &(VkFramebuffer){0}) == VK_ERROR_INITIALIZATION_FAILED);
    assert(fixture->submissions == before);
    destroy_fb(device, framebuffer, NULL);
    destroy_pass(device, other_pass, NULL);
    destroy_image(device, image, NULL);
    free_memory(device, memory, NULL);
    fixture->render_pass_info = NULL;
    fixture->framebuffer_info = NULL;
    fixture->image_info = NULL;
    fixture->view_info = NULL;
    fixture->memory_info = NULL;
}

/** @brief Canonical graphics compilation, validation and copied-definition ownership.
 * @param[in] device Live borrowed device; lookup static resolver.
 * @param[in,out] fixture Exclusive fake backend; all borrowed infos cleared before return.
 * @note No heap allocation. Both shader identities and creating layout/pass retire
 * before pipeline destruction, proving the compiled object owns scalar snapshots.
 */
/** @brief Exercise acknowledged graphics binding and deterministic profile reset/reuse.
 * @param[in] device/pipeline Live borrowed compatible device/pipeline identities.
 * @param[in] lookup Immutable dispatcher. @param[in,out] fixture Sole-thread backend.
 * @note Owns command pool/buffer until explicit retirement; borrowed info pointers cleared.
 */
static void graphics_bind_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup,
                                   fixture_t *fixture, VkPipeline pipeline) {
    PFN_vkCreateCommandPool create_pool =
        (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_pool =
        (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers allocate =
        (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers free_commands =
        (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
    PFN_vkResetCommandBuffer reset =
        (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
    PFN_vkCmdBindPipeline bind = (PFN_vkCmdBindPipeline)lookup(device, "vkCmdBindPipeline");
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    fixture->pool_info = &pool_info;
    VkCommandPool pool;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocation = {.sType =
                                                  VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool,
                                              .commandBufferCount = 1};
    fixture->command_allocate = &allocation;
    VkCommandBuffer command;
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fixture->command_begin = &begin_info;
    assert(allocate(device, &allocation, &command) == VK_SUCCESS);
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        assert(begin(command, &begin_info) == VK_SUCCESS);
        unsigned before = fixture->submissions;
        bind(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        assert(fixture->submissions == before + (iteration ? 1 : 2) && fixture->command == 93);
        assert(end(command) == VK_SUCCESS);
        assert(reset(command, 0) == VK_SUCCESS);
    }
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        assert(begin(command, &begin_info) == VK_SUCCESS);
        unsigned before = fixture->submissions;
        bind(command,
             scenario == 0   ? 2
             : scenario == 1 ? VK_PIPELINE_BIND_POINT_COMPUTE
                             : VK_PIPELINE_BIND_POINT_GRAPHICS,
             scenario == 2   ? NULL
             : scenario == 3 ? (VkPipeline)(uintptr_t)42
                             : pipeline);
        assert(fixture->submissions == before && end(command) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset(command, 0) == VK_SUCCESS);
    }
    free_commands(device, pool, 1, &command);
    assert(allocate(device, &allocation, &command) == VK_SUCCESS);
    assert(begin(command, &begin_info) == VK_SUCCESS);
    unsigned before = fixture->submissions;
    bind(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    assert(fixture->submissions == before + 1);
    assert(end(command) == VK_SUCCESS);
    destroy_pool(device, pool, NULL);
    fixture->pool_info = NULL;
    fixture->command_allocate = NULL;
    fixture->command_begin = NULL;
}

static void graphics_pipeline_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup,
                                       fixture_t *fixture) {
#define LoadGraphics(type, variable, name)                                                         \
    type variable = (type)lookup(device, name);                                                    \
    assert(variable)
    LoadGraphics(PFN_vkCreateRenderPass, create_pass, "vkCreateRenderPass");
    LoadGraphics(PFN_vkDestroyRenderPass, destroy_pass, "vkDestroyRenderPass");
    LoadGraphics(PFN_vkCreateShaderModule, create_shader, "vkCreateShaderModule");
    LoadGraphics(PFN_vkDestroyShaderModule, destroy_shader, "vkDestroyShaderModule");
    LoadGraphics(PFN_vkCreatePipelineLayout, create_layout, "vkCreatePipelineLayout");
    LoadGraphics(PFN_vkDestroyPipelineLayout, destroy_layout, "vkDestroyPipelineLayout");
    LoadGraphics(PFN_vkCreateGraphicsPipelines, create_pipeline, "vkCreateGraphicsPipelines");
    LoadGraphics(PFN_vkDestroyPipeline, destroy_pipeline, "vkDestroyPipeline");
#undef LoadGraphics
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass};
    fixture->render_pass_info = &pass_info;
    VkRenderPass pass;
    assert(create_pass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = sizeof(TriangleVertexShader),
                                            .pCode = TriangleVertexShader};
    fixture->shader_info = &shader_info;
    VkShaderModule vertex, fragment;
    assert(create_shader(device, &shader_info, NULL, &vertex) == VK_SUCCESS);
    shader_info.codeSize = sizeof(TriangleFragmentShader);
    shader_info.pCode = TriangleFragmentShader;
    assert(create_shader(device, &shader_info, NULL, &fragment) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = {.sType =
                                                  VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fixture->pipeline_layout_info = &layout_info;
    VkPipelineLayout layout;
    assert(create_layout(device, &layout_info, NULL, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {.width = 64, .height = 64, .maxDepth = 1};
    VkRect2D scissor = {.extent = {64, 64}};
    VkPipelineViewportStateCreateInfo viewport_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &vertex_info,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport_info,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &samples,
                                         .pColorBlendState = &blend,
                                         .layout = layout,
                                         .renderPass = pass,
                                         .basePipelineIndex = -1};
    fixture->graphics_info = &info;
    VkPipeline pipeline;
    unsigned before = fixture->submissions;
    assert(create_pipeline(device, NULL, 1, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    assert(create_pipeline(NULL, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    assert(create_pipeline(device, (VkPipelineCache)(uintptr_t)1, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    assert(create_pipeline(device, NULL, 2, (void *)(uintptr_t)1, NULL, (void *)(uintptr_t)1) ==
           VK_ERROR_INITIALIZATION_FAILED);
    assert(create_pipeline((VkDevice)(uintptr_t)42, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = (void *)(uintptr_t)1;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.pNext = NULL;
    info.pStages = NULL;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.pStages = stages;
    for (unsigned index = 0; index < 2; ++index) {
        VkShaderModule saved = stages[index].module;
        stages[index].module = NULL;
        assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
        stages[index].module = (VkShaderModule)(uintptr_t)42;
        assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
        stages[index].module = saved;
    }
    info.layout = NULL;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.layout = (VkPipelineLayout)(uintptr_t)42;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.layout = layout;
    info.renderPass = NULL;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.renderPass = (VkRenderPass)(uintptr_t)42;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_ERROR_INITIALIZATION_FAILED && !pipeline);
    info.renderPass = pass;
    info.flags = 1;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    info.flags = 0;
    info.stageCount = 1;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    info.stageCount = 2;
    stages[0].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    info.pDynamicState = (void *)(uintptr_t)1;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    info.pDynamicState = NULL;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    raster.rasterizerDiscardEnable = VK_TRUE;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    raster.rasterizerDiscardEnable = VK_FALSE;
    blend_attachment.blendEnable = VK_TRUE;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_INITIALIZATION_FAILED &&
           !pipeline);
    blend_attachment.blendEnable = VK_FALSE;
    assert(fixture->submissions == before);
    fixture->create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) ==
               VK_ERROR_OUT_OF_DEVICE_MEMORY &&
           !pipeline);
    fixture->create_result = VK_SUCCESS;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &pipeline) == VK_SUCCESS && pipeline);
    destroy_shader(device, vertex, NULL);
    destroy_shader(device, fragment, NULL);
    destroy_layout(device, layout, NULL);
    destroy_pass(device, pass, NULL);
    before = fixture->submissions;
    assert(create_pipeline(device, NULL, 1, &info, NULL, &(VkPipeline){0}) ==
           VK_ERROR_INITIALIZATION_FAILED);
    assert(fixture->submissions == before);
    graphics_bind_contract(device, lookup, fixture, pipeline);
    before = fixture->submissions;
    destroy_pipeline(device, pipeline, NULL);
    assert(fixture->submissions == before + 1);
    destroy_pipeline(device, pipeline, NULL);
    assert(fixture->submissions == before + 1);
    fixture->render_pass_info = NULL;
    fixture->shader_info = NULL;
    fixture->pipeline_layout_info = NULL;
    fixture->graphics_info = NULL;
}

/** @brief Standalone render-pass begin/end frontend contract, no draw/copy APIs required.
 * @param[in] device Live borrowed device; lookup immutable device resolver.
 * @param[in,out] fixture Sole-thread fake backend; create infos borrowed only during calls.
 * @note Owns and deterministically retires the complete target/pool graph; no heap.
 * Invalid void ordering invalidates recording locally. Success requires primary graphics
 * command buffer, compatible live attachments, supported usage/layout and bounded clear.
 */
static void renderpass_begin_end_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup,
                                          fixture_t *fixture) {
    PFN_vkCreateRenderPass create_pass =
        (PFN_vkCreateRenderPass)lookup(device, "vkCreateRenderPass");
    PFN_vkDestroyRenderPass destroy_pass =
        (PFN_vkDestroyRenderPass)lookup(device, "vkDestroyRenderPass");
    PFN_vkCreateFramebuffer create_fb =
        (PFN_vkCreateFramebuffer)lookup(device, "vkCreateFramebuffer");
    PFN_vkDestroyFramebuffer destroy_fb =
        (PFN_vkDestroyFramebuffer)lookup(device, "vkDestroyFramebuffer");
    PFN_vkCreateImage create_image = (PFN_vkCreateImage)lookup(device, "vkCreateImage");
    PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)lookup(device, "vkDestroyImage");
    PFN_vkGetImageMemoryRequirements requirements =
        (PFN_vkGetImageMemoryRequirements)lookup(device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)lookup(device, "vkBindImageMemory");
    PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)lookup(device, "vkCreateImageView");
    PFN_vkDestroyImageView destroy_view =
        (PFN_vkDestroyImageView)lookup(device, "vkDestroyImageView");
    assert(create_fb && destroy_fb);
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                          .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass};
    fixture->render_pass_info = &pass_info;
    VkRenderPass pass;
    assert(create_pass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .extent = {64, 64, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    fixture->image_info = &image_info;
    VkImage image;
    assert(create_image(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements memory_requirements = {0};
    requirements(device, image, &memory_requirements);
    assert(memory_requirements.size);
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .allocationSize = memory_requirements.size};
    fixture->memory_info = &memory_info;
    VkDeviceMemory memory;
    assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS);
    assert(bind(device, image, memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = image,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    fixture->view_info = &view_info;
    VkImageView view;
    assert(create_view(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = pass,
                                    .attachmentCount = 1,
                                    .pAttachments = &view,
                                    .width = 64,
                                    .height = 64,
                                    .layers = 1};
    fixture->framebuffer_info = &info;
#define LoadPass(type, variable, name)                                                             \
    type variable = (type)lookup(device, name);                                                    \
    assert(variable)
    LoadPass(PFN_vkCreateCommandPool, create_pool, "vkCreateCommandPool");
    LoadPass(PFN_vkDestroyCommandPool, destroy_pool, "vkDestroyCommandPool");
    LoadPass(PFN_vkAllocateCommandBuffers, allocate_commands, "vkAllocateCommandBuffers");
    LoadPass(PFN_vkBeginCommandBuffer, begin, "vkBeginCommandBuffer");
    LoadPass(PFN_vkEndCommandBuffer, end, "vkEndCommandBuffer");
    LoadPass(PFN_vkResetCommandBuffer, reset, "vkResetCommandBuffer");
    LoadPass(PFN_vkCmdBeginRenderPass, begin_pass, "vkCmdBeginRenderPass");
    LoadPass(PFN_vkCmdEndRenderPass, end_pass, "vkCmdEndRenderPass");
    LoadPass(PFN_vkCmdFillBuffer, fill, "vkCmdFillBuffer");
    LoadPass(PFN_vkCmdUpdateBuffer, update, "vkCmdUpdateBuffer");
    LoadPass(PFN_vkCmdCopyBuffer, copy_buffer, "vkCmdCopyBuffer");
    LoadPass(PFN_vkCmdDispatch, dispatch, "vkCmdDispatch");
    LoadPass(PFN_vkCmdPipelineBarrier, barrier, "vkCmdPipelineBarrier");
    LoadPass(PFN_vkCreateBuffer, create_buffer, "vkCreateBuffer");
    LoadPass(PFN_vkDestroyBuffer, destroy_buffer, "vkDestroyBuffer");
    LoadPass(PFN_vkBindBufferMemory, bind_buffer, "vkBindBufferMemory");
#undef LoadPass
    VkFramebuffer framebuffer;
    assert(create_fb(device, &info, NULL, &framebuffer) == VK_SUCCESS);
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    fixture->pool_info = &pool_info;
    VkCommandPool pool;
    assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocation = {.sType =
                                                  VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool,
                                              .commandBufferCount = 1};
    fixture->command_allocate = &allocation;
    VkCommandBuffer command;
    assert(allocate_commands(device, &allocation, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fixture->command_begin = &begin_info;
    VkClearValue clear = {.color = {{0, 0, 0, 1}}};
    VkRenderPassBeginInfo render_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                         .renderPass = pass,
                                         .framebuffer = framebuffer,
                                         .renderArea = {.extent = {64, 64}},
                                         .clearValueCount = 1,
                                         .pClearValues = &clear};
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = 256,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    fixture->buffer_info = &buffer_info;
    VkBuffer transfer_buffer;
    assert(create_buffer(device, &buffer_info, NULL, &transfer_buffer) == VK_SUCCESS);
    assert(bind_buffer(device, transfer_buffer, memory, 0) == VK_SUCCESS);
    /* Warm immutable graphics-family cache without assuming number of query packets. */
    assert(begin(command, &begin_info) == VK_SUCCESS);
    begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    end_pass(command);
    assert(end(command) == VK_SUCCESS);
    assert(reset(command, 0) == VK_SUCCESS);
    unsigned ignored_before = fixture->submissions;
    begin_pass(NULL, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    begin_pass((VkCommandBuffer)(uintptr_t)42, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    end_pass(command);
    assert(fixture->submissions == ignored_before);
    attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    VkRenderPass incompatible_pass;
    assert(create_pass(device, &pass_info, NULL, &incompatible_pass) == VK_SUCCESS);
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    for (unsigned scenario = 0; scenario < 25; ++scenario) {
        assert(begin(command, &begin_info) == VK_SUCCESS);
        unsigned before = fixture->submissions;
        if (scenario == 0)
            end_pass(command);
        if (scenario == 1) {
            render_info.renderPass = incompatible_pass;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderPass = pass;
        }
        if (scenario == 2) {
            render_info.renderArea.extent.width = 65;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderArea.extent.width = 64;
        }
        if (scenario == 3) {
            render_info.renderArea.offset.x = -1;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderArea.offset.x = 0;
        }
        if (scenario == 4) {
            render_info.pClearValues = NULL;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.pClearValues = &clear;
        }
        if (scenario == 5)
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        if (scenario == 6) {
            render_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        }
        if (scenario == 7) {
            render_info.pNext = (void *)(uintptr_t)1;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.pNext = NULL;
        }
        if (scenario == 15) {
            render_info.clearValueCount = 0;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.clearValueCount = 1;
        }
        if (scenario == 16) {
            render_info.renderArea.extent.height = 0;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderArea.extent.height = 64;
        }
        if (scenario == 17) {
            render_info.framebuffer = NULL;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.framebuffer = framebuffer;
        }
        if (scenario == 18) begin_pass(command, NULL, VK_SUBPASS_CONTENTS_INLINE);
        if (scenario == 19 || scenario == 20) {
            render_info.renderPass = scenario == 19 ? NULL : (VkRenderPass)(uintptr_t)42;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderPass = pass;
        }
        if (scenario == 21) {
            render_info.framebuffer = (VkFramebuffer)(uintptr_t)42;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.framebuffer = framebuffer;
        }
        if (scenario == 22 || scenario == 23) {
            render_info.renderArea.offset.x = scenario == 22 ? 65 : 0;
            render_info.renderArea.offset.y = scenario == 23 ? 65 : 0;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderArea.offset.x = render_info.renderArea.offset.y = 0;
        }
        if (scenario == 24) {
            render_info.renderArea.extent.height = 65;
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            render_info.renderArea.extent.height = 64;
        }
        if (scenario >= 8 && scenario < 15) {
            begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            before = fixture->submissions;
            if (scenario == 8)
                begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
            if (scenario == 10)
                dispatch(command, 1, 1, 1);
            if (scenario == 11)
                fill(command, transfer_buffer, 0, 4, 0);
            if (scenario == 12) {
                const uint32_t word = 0;
                update(command, transfer_buffer, 0, 4, &word);
            }
            if (scenario == 13) {
                const VkBufferCopy region = {.dstOffset = 4, .size = 4};
                copy_buffer(command, transfer_buffer, transfer_buffer, 1, &region);
            }
            if (scenario == 14)
                barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
            /* scenario9 attempts EndCommandBuffer while pass remains active. */
        }
        assert(fixture->submissions == before);
        assert(end(command) == VK_ERROR_INITIALIZATION_FAILED);
        assert(fixture->submissions == before);
        assert(reset(command, 0) == VK_SUCCESS);
    }
    /* The secondary token itself is supported, but this recording profile is primary only. */
    allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    VkCommandBuffer secondary;
    assert(allocate_commands(device, &allocation, &secondary) == VK_SUCCESS);
    unsigned before = fixture->submissions;
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    begin_info.pInheritanceInfo = &inheritance;
    assert(begin(secondary, &begin_info) == VK_SUCCESS);
    before = fixture->submissions;
    begin_pass(secondary, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    assert(fixture->submissions == before);
    assert(end(secondary) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset(secondary, 0) == VK_SUCCESS);
    begin_info.pInheritanceInfo = NULL;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    before = fixture->submissions;
    end_pass(NULL);
    end_pass((VkCommandBuffer)(uintptr_t)1);
    assert(fixture->submissions == before);
    /* SHADER_READ_ONLY final layout requires image SAMPLED or INPUT_ATTACHMENT usage. */
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkRenderPass sampled_pass;
    assert(create_pass(device, &pass_info, NULL, &sampled_pass) == VK_SUCCESS);
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    render_info.renderPass = sampled_pass;
    assert(begin(command, &begin_info) == VK_SUCCESS);
    before = fixture->submissions;
    begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    assert(fixture->submissions == before);
    assert(end(command) == VK_ERROR_INITIALIZATION_FAILED);
    assert(reset(command, 0) == VK_SUCCESS);
    image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage sampled_image;
    assert(create_image(device, &image_info, NULL, &sampled_image) == VK_SUCCESS);
    assert(bind(device, sampled_image, memory, 0) == VK_SUCCESS);
    view_info.image = sampled_image;
    VkImageView sampled_view;
    assert(create_view(device, &view_info, NULL, &sampled_view) == VK_SUCCESS);
    info.renderPass = sampled_pass;
    info.pAttachments = &sampled_view;
    VkFramebuffer sampled_framebuffer;
    assert(create_fb(device, &info, NULL, &sampled_framebuffer) == VK_SUCCESS);
    render_info.framebuffer = sampled_framebuffer;
    assert(begin(command, &begin_info) == VK_SUCCESS);
    begin_pass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    end_pass(command);
    assert(end(command) == VK_SUCCESS);
    assert(reset(command, 0) == VK_SUCCESS);
    destroy_buffer(device, transfer_buffer, NULL);
    destroy_pool(device, pool, NULL);
    destroy_fb(device, sampled_framebuffer, NULL);
    destroy_view(device, sampled_view, NULL);
    destroy_image(device, sampled_image, NULL);
    destroy_pass(device, sampled_pass, NULL);
    destroy_fb(device, framebuffer, NULL);
    destroy_view(device, view, NULL);
    destroy_image(device, image, NULL);
    free_memory(device, memory, NULL);
    destroy_pass(device, pass, NULL);
    destroy_pass(device, incompatible_pass, NULL);
    fixture->buffer_info = NULL;
    fixture->render_pass_info = NULL;
    fixture->framebuffer_info = NULL;
    fixture->image_info = NULL;
    fixture->view_info = NULL;
    fixture->memory_info = NULL;
    fixture->pool_info = NULL;
    fixture->command_allocate = NULL;
    fixture->command_begin = NULL;
}

static void image_contract(void) {
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
        fixture.device_info = &device_info;
        VkDevice device = NULL;
        assert(((PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice"))(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        if (scenario == 0) { render_pass_contract(device, lookup, &fixture); framebuffer_contract(device, lookup, &fixture); graphics_pipeline_contract(device, lookup, &fixture); renderpass_begin_end_contract(device, lookup, &fixture); shader_contract(device, lookup, &fixture); descriptor_lifecycle_contract(device, lookup, &fixture); layout_contract(device, lookup, &fixture); compute_pipeline_contract(device, lookup, &fixture); }
        PFN_vkCreateImage create_image = (PFN_vkCreateImage)lookup(device, "vkCreateImage");
        PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)lookup(device, "vkDestroyImage");
        PFN_vkGetImageMemoryRequirements requirements = (PFN_vkGetImageMemoryRequirements)lookup(device, "vkGetImageMemoryRequirements");
        PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)lookup(device, "vkBindImageMemory");
        PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)lookup(device, "vkCreateImageView");
        PFN_vkDestroyImageView destroy_view = (PFN_vkDestroyImageView)lookup(device, "vkDestroyImageView");
        assert(create_image && destroy_image && requirements && bind && create_view && destroy_view);
        VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = 1,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        fixture.image_info = &info;
        VkImage image = NULL;
        unsigned before = fixture.submissions;
        assert(create_image(device, NULL, NULL, &image) == VK_ERROR_INITIALIZATION_FAILED && !image);
        assert(create_image(NULL, &info, NULL, &image) == VK_ERROR_INITIALIZATION_FAILED && !image);
        assert(create_image(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(fixture.submissions == before);
        info.flags = 1;
        assert(create_image(device, &info, NULL, &image) == VK_ERROR_INITIALIZATION_FAILED && !image);
        info.flags = 0;
        info.mipLevels = 6;
        assert(create_image(device, &info, NULL, &image) == VK_ERROR_INITIALIZATION_FAILED && !image);
        info.mipLevels = 1;
        info.samples = 2; info.mipLevels = 2;
        assert(create_image(device, &info, NULL, &image) == VK_ERROR_INITIALIZATION_FAILED && !image);
        info.samples = 1; info.mipLevels = 1;
        destroy_image(NULL, image, NULL);
        destroy_image(device, NULL, NULL);
        destroy_image(device, (VkImage)(uintptr_t)1, NULL);
        if (scenario == 1) fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (scenario == 2) fixture.corrupt_command = 54;
        if (scenario == 3) fixture.fail_command = 54;
        VkResult created = create_image(device, &info, NULL, &image);
        if (scenario == 2 || scenario == 3) {
            assert(created == VK_ERROR_DEVICE_LOST && !image);
            venus_icd_abandon();
            continue;
        }
        if (scenario == 1) {
            assert(created == VK_ERROR_OUT_OF_DEVICE_MEMORY && !image);
            fixture.create_result = VK_SUCCESS;
            assert(create_image(device, &info, NULL, &image) == VK_SUCCESS && image);
        } else assert(created == VK_SUCCESS && image);
        VkMemoryRequirements value = {0};
        requirements(NULL, image, &value);
        requirements(device, NULL, &value);
        requirements(device, image, NULL);
        requirements(device, (VkImage)(uintptr_t)1, &value);
        assert(!value.size);
        if (scenario == 4) fixture.requirements_fault = 4;
        requirements(device, image, &value);
        if (scenario == 4) {
            assert(!value.size);
            venus_icd_abandon();
            continue;
        }
        assert(value.size && value.alignment == 256);
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = value.size + 256, .memoryTypeIndex = 0};
        fixture.memory_info = &allocation;
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        VkDeviceMemory memory = NULL;
        assert(allocate(device, &allocation, NULL, &memory) == VK_SUCCESS);
        assert(bind(NULL, image, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, NULL, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, image, NULL, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, (VkImage)(uintptr_t)1, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, image, (VkDeviceMemory)(uintptr_t)1, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, image, memory, allocation.allocationSize) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, image, memory, 1) == VK_ERROR_INITIALIZATION_FAILED);
        if (scenario == 5) fixture.bind_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(bind(device, image, memory, 0) == (scenario == 5 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS));
        fixture.bind_result = VK_SUCCESS;
        if (scenario == 5) assert(bind(device, image, memory, 0) == VK_SUCCESS);
        assert(bind(device, image, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        before = fixture.submissions;
        release(device, memory, NULL);
        assert(fixture.submissions == before);
        VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = info.format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
        fixture.view_info = &view_info;
        VkImageView view = NULL;
        assert(create_view(device, &view_info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_view(NULL, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_view(device, NULL, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.image = (VkImage)(uintptr_t)1;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.image = image;
        view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.format = info.format;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.components.r = 7;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.components.r = 0;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED);
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 1;
        assert(create_view(device, &view_info, NULL, &view) == VK_ERROR_INITIALIZATION_FAILED && !view);
        view_info.subresourceRange.baseMipLevel = 0;
        if (scenario == 6) fixture.create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        assert(create_view(device, &view_info, NULL, &view) == (scenario == 6 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS));
        fixture.create_result = VK_SUCCESS;
        if (scenario == 6) assert(create_view(device, &view_info, NULL, &view) == VK_SUCCESS);
        if (scenario == 0) {
            PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
            PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
            PFN_vkAllocateCommandBuffers allocate_commands = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
            PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
            PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
            PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)lookup(device, "vkCmdPipelineBarrier");
            VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
            fixture.pool_info = &pool_info;
            VkCommandPool pool = NULL;
            assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
            VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool, .commandBufferCount = 1};
            fixture.command_allocate = &command_info;
            VkCommandBuffer recording = NULL;
            assert(allocate_commands(device, &command_info, &recording) == VK_SUCCESS);
            VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            fixture.command_begin = &begin_info;
            const VkImageMemoryBarrier initial = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
            for (unsigned invalid = 0; invalid < 12; invalid++) {
                VkImageMemoryBarrier item = initial;
                if (invalid == 2) item.sType = 0;
                if (invalid == 3) item.pNext = &item;
                if (invalid == 4) item.image = NULL;
                if (invalid == 5) item.image = (VkImage)(uintptr_t)1;
                if (invalid == 6) item.subresourceRange.levelCount = 2;
                if (invalid == 7) item.srcQueueFamilyIndex = 0;
                if (invalid == 8) item.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (invalid == 9) item.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (invalid == 10) item.dstAccessMask = UINT32_MAX;
                assert(begin(recording, &begin_info) == VK_SUCCESS);
                unsigned submitted = fixture.submissions;
                if (invalid == 11)
                    barrier(recording, 1, 4096, 0, 64, (const void *)(uintptr_t)8,
                            64, (const void *)(uintptr_t)8, 64, (const void *)(uintptr_t)8);
                else barrier(recording, 1, 4096, 0, 0, NULL, 0, NULL, 1, invalid == 1 ? NULL : &item);
                assert(fixture.submissions == submitted + (invalid == 0));
                assert(end(recording) == (invalid == 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED));
            }
            destroy_pool(device, pool, NULL);
        }
        before = fixture.submissions;
        destroy_image(device, image, NULL);
        assert(fixture.submissions == before);
        if (scenario == 7) fixture.corrupt_command = 58;
        destroy_view(device, view, NULL);
        if (scenario == 7) {
            venus_icd_abandon();
            continue;
        }
        destroy_image(device, image, NULL);
        release(device, memory, NULL);
        ((PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice"))(device, NULL);
        destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}

static void buffer_contract(void) {
    for (unsigned scenario = 0; scenario < 15; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queues[2] = {
            {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
             .queueCount = 1, .pQueuePriorities = &priority},
            {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 1,
             .queueCount = 1, .pQueuePriorities = &priority}};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 2, .pQueueCreateInfos = queues};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkGetBufferMemoryRequirements requirements = (PFN_vkGetBufferMemoryRequirements)lookup(device, "vkGetBufferMemoryRequirements");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        fixture.buffer_info = &info;
        VkBuffer buffer = NULL;
        assert(create_buffer(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_buffer(NULL, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_buffer((VkDevice)(uintptr_t)1, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_buffer(device, NULL, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = 0;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.pNext = &info;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.pNext = NULL; info.flags = 1;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.flags = 0; info.size = 0;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.size = 4096; info.usage = 0;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.usage = 0x200;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; info.sharingMode = 99;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        uint32_t families[2] = {0, 1};
        info.sharingMode = VK_SHARING_MODE_CONCURRENT; info.queueFamilyIndexCount = 1;
        info.pQueueFamilyIndices = families;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.queueFamilyIndexCount = 17;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.queueFamilyIndexCount = 2; info.pQueueFamilyIndices = NULL;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        info.pQueueFamilyIndices = families; families[1] = 0;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        families[1] = 99;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_INITIALIZATION_FAILED);
        families[1] = 1;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_SUCCESS && buffer);
        destroy_buffer(device, buffer, NULL);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.queueFamilyIndexCount = UINT32_MAX; info.pQueueFamilyIndices = NULL;
        fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !buffer);
        fixture.create_result = VK_SUCCESS;
        if (scenario == 6 || scenario == 7) {
            fixture.corrupt_command = scenario == 6 ? 50 : UINT32_MAX;
            fixture.fail_command = scenario == 7 ? 50 : UINT32_MAX;
            assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_DEVICE_LOST && !buffer);
            venus_icd_abandon(); continue;
        }
        if (scenario == 13 || scenario == 14) {
            fixture.create_result = scenario == 13 ? VK_ERROR_DEVICE_LOST : VK_NOT_READY;
            assert(create_buffer(device, &info, NULL, &buffer) == VK_ERROR_DEVICE_LOST && !buffer);
            venus_icd_abandon(); continue;
        }
        assert(create_buffer(device, &info, NULL, &buffer) == VK_SUCCESS && buffer);
        destroy_device(device, NULL);
        assert(lookup(device, "vkCreateBuffer"));
        destroy_buffer(NULL, buffer, NULL); destroy_buffer(device, NULL, NULL);
        destroy_buffer((VkDevice)(uintptr_t)1, buffer, NULL); destroy_buffer(foreign, buffer, NULL);
        VkMemoryRequirements value = {0};
        requirements(NULL, buffer, &value); requirements(device, NULL, &value);
        requirements((VkDevice)(uintptr_t)1, buffer, &value); requirements(foreign, buffer, &value);
        requirements(device, buffer, NULL);
        requirements(device, buffer, &value);
        assert(value.size == 8192 && value.alignment == 256 && value.memoryTypeBits == 7);
        memset(&value, 0xa5, sizeof(value));
        VkMemoryRequirements saved; memcpy(&saved, &value, sizeof(saved));
        if (scenario < 6) fixture.requirements_fault = scenario + 1;
        if (scenario == 8) fixture.corrupt_command = 30;
        if (scenario == 9) fixture.fail_command = 30;
        if (scenario < 6 || scenario == 8 || scenario == 9) {
            requirements(device, buffer, &value);
            assert(!memcmp(&saved, &value, sizeof(saved)));
            venus_icd_abandon(); continue;
        }
        if (scenario == 10) fixture.corrupt_command = 51;
        if (scenario == 11) fixture.fail_command = 51;
        if (scenario == 12) {
            VkBuffer buffers[502];
            for (unsigned index = 0; index < 502; index++)
                assert(create_buffer(device, &info, NULL, &buffers[index]) == VK_SUCCESS);
            VkBuffer exhausted = NULL;
            assert(create_buffer(device, &info, NULL, &exhausted) == VK_ERROR_OUT_OF_HOST_MEMORY && !exhausted);
            for (unsigned index = 0; index < 502; index++) destroy_buffer(device, buffers[index], NULL);
        }
        destroy_buffer(device, buffer, NULL);
        if (scenario == 10 || scenario == 11) { venus_icd_abandon(); continue; }
        requirements(device, buffer, &value);
        assert(!memcmp(&saved, &value, sizeof(saved)));
        destroy_buffer(device, buffer, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void memory_contract(void) {
    for (unsigned scenario = 0; scenario < 13; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 16384, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate(device, &memory_info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate(NULL, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate((VkDevice)(uintptr_t)1, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate(device, NULL, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        memory_info.sType = 0;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; memory_info.pNext = &memory_info;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        memory_info.pNext = NULL; memory_info.allocationSize = 0;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        memory_info.allocationSize = 16384; memory_info.memoryTypeIndex = 32;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_INITIALIZATION_FAILED);
        memory_info.memoryTypeIndex = 0; fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !memory);
        fixture.create_result = VK_SUCCESS;
        if (scenario < 4) {
            if (scenario == 0) fixture.fail_command = 21;
            if (scenario == 1) fixture.corrupt_command = 21;
            if (scenario == 2) fixture.create_result = VK_ERROR_DEVICE_LOST;
            if (scenario == 3) fixture.create_result = VK_NOT_READY;
            assert(allocate(device, &memory_info, NULL, &memory) == VK_ERROR_DEVICE_LOST && !memory);
            venus_icd_abandon(); continue;
        }
        if (scenario == 12) memory_info.memoryTypeIndex = 3;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS && memory);
        VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        fixture.buffer_info = &info;
        VkBuffer buffer = NULL;
        assert(create_buffer(device, &info, NULL, &buffer) == VK_SUCCESS);
        release(NULL, memory, NULL); release(device, NULL, NULL);
        release((VkDevice)(uintptr_t)1, memory, NULL); release(foreign, memory, NULL);
        assert(bind(NULL, buffer, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, NULL, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, buffer, NULL, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind((VkDevice)(uintptr_t)1, buffer, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(foreign, buffer, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, (VkBuffer)(uintptr_t)1, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, buffer, (VkDeviceMemory)(uintptr_t)1, 0) == VK_ERROR_INITIALIZATION_FAILED);
        if (scenario == 12) {
            assert(bind(device, buffer, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
            destroy_buffer(device, buffer, NULL); release(device, memory, NULL);
            destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
            assert(venus_icd_unbind() == RingOk); continue;
        }
        if (scenario == 8) {
            fixture.fail_command = 30;
            assert(bind(device, buffer, memory, 0) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        if (scenario == 9) {
            VkDeviceMemory allocations[503];
            for (unsigned index = 0; index < 503; index++)
                assert(allocate(device, &memory_info, NULL, &allocations[index]) == VK_SUCCESS);
            VkDeviceMemory exhausted = NULL;
            assert(allocate(device, &memory_info, NULL, &exhausted) == VK_ERROR_OUT_OF_HOST_MEMORY && !exhausted);
            for (unsigned index = 0; index < 503; index++) release(device, allocations[index], NULL);
        }
        assert(bind(device, buffer, memory, 1) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, buffer, memory, UINT64_MAX - 255) == VK_ERROR_INITIALIZATION_FAILED);
        assert(bind(device, buffer, memory, 16384) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.bind_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(bind(device, buffer, memory, 0) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.bind_result = VK_SUCCESS;
        if (scenario == 4) fixture.fail_command = 28;
        if (scenario == 5) fixture.corrupt_command = 28;
        if (scenario == 10) fixture.bind_result = VK_ERROR_DEVICE_LOST;
        if (scenario == 11) fixture.bind_result = VK_NOT_READY;
        if (scenario == 4 || scenario == 5 || scenario == 10 || scenario == 11) {
            assert(bind(device, buffer, memory, 0) == VK_ERROR_DEVICE_LOST);
            assert(bind(device, buffer, memory, 0) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        assert(bind(device, buffer, memory, 256) == VK_SUCCESS);
        assert(bind(device, buffer, memory, 0) == VK_ERROR_INITIALIZATION_FAILED);
        unsigned submissions = fixture.submissions;
        release(device, memory, NULL);
        assert(submissions == fixture.submissions);
        destroy_buffer(device, buffer, NULL);
        if (scenario == 6) fixture.fail_command = 22;
        if (scenario == 7) fixture.corrupt_command = 22;
        release(device, memory, NULL);
        if (scenario == 6 || scenario == 7) { venus_icd_abandon(); continue; }
        release(device, memory, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void mapping_contract(void) {
    for (unsigned scenario = 0; scenario < 17; scenario++) {
        fixture_t fixture = fresh(); fixture.mapping_enabled = 1;
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2; VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkMapMemory map = (PFN_vkMapMemory)lookup(device, "vkMapMemory");
        PFN_vkUnmapMemory unmap = (PFN_vkUnmapMemory)lookup(device, "vkUnmapMemory");
        PFN_vkFlushMappedMemoryRanges flush = (PFN_vkFlushMappedMemoryRanges)lookup(device, "vkFlushMappedMemoryRanges");
        PFN_vkInvalidateMappedMemoryRanges invalidate = (PFN_vkInvalidateMappedMemoryRanges)lookup(device, "vkInvalidateMappedMemoryRanges");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        assert(map && unmap && flush && invalidate);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 16384, .memoryTypeIndex = 0};
        if (scenario == 13) memory_info.allocationSize = 16777217;
        if (scenario == 15) memory_info.allocationSize = 16383;
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS);
        void *pointer = (void *)(uintptr_t)1;
        assert(map(device, memory, 0, 1, 0, NULL) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(NULL, memory, 0, 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED && !pointer);
        assert(map((VkDevice)(uintptr_t)1, memory, 0, 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, NULL, 0, 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(foreign, memory, 0, 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, memory, 0, 1, 1, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, memory, 0, 0, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, memory, memory_info.allocationSize, 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, memory, 1, UINT64_MAX - 1, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED);
        if (scenario == 14) {
            VkDeviceMemory allocations[65] = {memory};
            for (unsigned index = 1; index < 65; index++)
                assert(allocate(device, &memory_info, NULL, &allocations[index]) == VK_SUCCESS);
            for (unsigned index = 0; index < 64; index++)
                assert(map(device, allocations[index], 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS);
            assert(fixture.mapping_creates == 64);
            assert(map(device, allocations[64], 0, VK_WHOLE_SIZE, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED && !pointer);
            unmap(device, allocations[0]);
            assert(map(device, allocations[64], 0, VK_WHOLE_SIZE, 0, &pointer) == VK_ERROR_MEMORY_MAP_FAILED && !pointer);
            release(device, allocations[0], NULL);
            allocations[0] = NULL;
            assert(map(device, allocations[64], 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS);
            assert(fixture.mapping_creates == 65 && fixture.mapping_frees == 1);
            for (unsigned index = 1; index < 65; index++) release(device, allocations[index], NULL);
            assert(fixture.mapping_frees == 65);
            destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
            assert(venus_icd_unbind() == RingOk); continue;
        }
        if (scenario == 15) {
            assert(map(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS);
            assert(fixture.mapping_creates == 1 && fixture.mapping_reads == 4);
            memset(pointer, 0x7f, 16383);
            fixture.mapping_storage[16383] = 0xa7;
            VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = memory, .size = VK_WHOLE_SIZE};
            assert(flush(device, 1, &range) == VK_SUCCESS);
            assert(fixture.mapping_writes == 4 && fixture.mapping_storage[16382] == 0x7f &&
                fixture.mapping_storage[16383] == 0xa7);
            memset(fixture.mapping_storage, 0x6b, 16383);
            assert(invalidate(device, 1, &range) == VK_SUCCESS);
            assert(((unsigned char *)pointer)[16382] == 0x6b);
            unmap(device, memory);
            assert(map(device, memory, 16382, 1, 0, &pointer) == VK_SUCCESS);
            assert(*(unsigned char *)pointer == 0x6b && fixture.mapping_creates == 1);
            release(device, memory, NULL);
            assert(fixture.mapping_frees == 1);
            destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
            assert(venus_icd_unbind() == RingOk); continue;
        }
        if (scenario == 16) {
            fixture.mapping_corrupt = 2;
            assert(map(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_ERROR_DEVICE_LOST && !pointer);
            assert(fixture.mapping_creates == 1 && fixture.mapping_reads == 1);
            venus_icd_abandon(); continue;
        }
#if defined(VgpuMappingAllocationFaults) && !defined(_WIN32)
        if (scenario == 0) {
            atomic_store_explicit(&mapping_fail_allocation, 1, memory_order_relaxed);
            assert(map(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_ERROR_OUT_OF_HOST_MEMORY && !pointer);
            assert(!atomic_load_explicit(&mapping_fail_allocation, memory_order_relaxed));
            assert(fixture.mapping_creates == 0 && fixture.mapping_reads == 0);
        }
#endif
        if (scenario == 1) fixture.mapping_noncoherent = 1;
        if (scenario == 2) { fixture.mapping_fail_kind = RequestCreate; fixture.mapping_failure = RingLimit; }
        if (scenario == 3) { fixture.mapping_fail_kind = RequestCreate; fixture.mapping_failure = RingCorrupt; }
        if (scenario == 4) { fixture.mapping_fail_kind = RequestRead; fixture.mapping_failure = RingInvalid; }
        if (scenario == 5) fixture.mapping_corrupt = 1;
        if (scenario == 6) fixture.corrupt_command = 8;
        if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 4 || scenario == 5 || scenario == 6 || scenario == 13) {
            VkResult expected = scenario == 3 || scenario == 5 || scenario == 6 ? VK_ERROR_DEVICE_LOST : VK_ERROR_MEMORY_MAP_FAILED;
            assert(map(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == expected && !pointer);
            venus_icd_abandon(); continue;
        }
        assert(map(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS && pointer);
        assert((uintptr_t)pointer % 4096 == 0 && fixture.mapping_reads == 4);
        void *duplicate = NULL;
        assert(map(device, memory, 0, 1, 0, &duplicate) == VK_ERROR_MEMORY_MAP_FAILED && !duplicate);
        memset(pointer, 0x5a, 16384);
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE};
        assert(flush(NULL, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(flush((VkDevice)(uintptr_t)1, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(flush(foreign, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(flush(device, 65, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(flush(device, 1, NULL) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(flush(device, 0, NULL) == VK_SUCCESS);
        for (unsigned invalid = 0; invalid < 9; invalid++) {
            VkMappedMemoryRange invalid_range = range;
            if (invalid == 0) invalid_range.sType = 0;
            if (invalid == 1) invalid_range.pNext = &range;
            if (invalid == 2) invalid_range.memory = NULL;
            if (invalid == 3) invalid_range.memory = (VkDeviceMemory)(uintptr_t)1;
            if (invalid == 4) invalid_range.offset = 16384;
            if (invalid == 5) invalid_range.size = 0;
            if (invalid == 6) invalid_range.size = 16385;
            if (invalid == 7) invalid_range.offset = UINT64_MAX;
            if (invalid == 8) invalid_range.size = UINT64_MAX - 1;
            unsigned writes = fixture.mapping_writes;
            VkMappedMemoryRange ranges[2] = {range, invalid_range};
            assert(flush(device, 2, ranges) == VK_ERROR_MEMORY_MAP_FAILED && writes == fixture.mapping_writes);
        }
        if (scenario == 7 || scenario == 8) { fixture.mapping_fail_kind = RequestWrite; fixture.mapping_failure = scenario == 7 ? RingInvalid : RingClosed; }
        VkResult expected_flush = scenario == 7 ? VK_ERROR_MEMORY_MAP_FAILED : scenario == 8 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        assert(flush(device, 1, &range) == expected_flush);
        if (scenario == 8) { venus_icd_abandon(); continue; }
        fixture.mapping_fail_kind = 0;
        assert(flush(device, 1, &range) == VK_SUCCESS);
        assert(fixture.mapping_storage[0] == 0x5a && fixture.mapping_storage[16383] == 0x5a);
        memset(fixture.mapping_storage, 0x37, 16384);
        if (scenario == 9) { fixture.mapping_fail_kind = RequestRead; fixture.mapping_failure = RingTimeout; }
        if (scenario == 10) fixture.mapping_corrupt = 1;
        assert(invalidate(device, 1, &range) == (scenario == 9 || scenario == 10 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS));
        if (scenario == 9 || scenario == 10) { venus_icd_abandon(); continue; }
        assert(((unsigned char *)pointer)[0] == 0x37 && ((unsigned char *)pointer)[16383] == 0x37);
        unmap(NULL, memory); unmap((VkDevice)(uintptr_t)1, memory); unmap(device, NULL); unmap(foreign, memory);
        unmap(device, memory); unmap(device, memory);
        assert(flush(device, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(map(device, memory, 4096, 8192, 0, &pointer) == VK_SUCCESS && fixture.mapping_creates == 1);
        range.offset = 0; range.size = 4096;
        assert(flush(device, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        range.offset = 4096; range.size = VK_WHOLE_SIZE;
        assert(flush(device, 1, &range) == VK_ERROR_MEMORY_MAP_FAILED);
        range.size = 8192;
        assert(flush(device, 1, &range) == VK_SUCCESS);
        if (scenario == 11) { fixture.mapping_fail_kind = RequestFree; fixture.mapping_failure = RingCorrupt; }
        if (scenario == 12) fixture.fail_command = 22;
        release(device, memory, NULL);
        if (scenario == 11 || scenario == 12) { venus_icd_abandon(); continue; }
        assert(fixture.mapping_frees == 1);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void pool_contract(void) {
    for (unsigned scenario = 0; scenario < 13; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2; VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        fixture.pool_info = &info;
        VkCommandPool pool = NULL;
        assert(create_pool(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_pool(NULL, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_pool((VkDevice)(uintptr_t)1, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_pool(device, NULL, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = 0;
        assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; info.pNext = &info;
        assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        info.pNext = NULL; info.flags = 4;
        assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        info.flags = 0; info.queueFamilyIndex = 99;
        assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_INITIALIZATION_FAILED);
        info.queueFamilyIndex = 0;
        for (info.flags = 0; info.flags <= 3; info.flags++) {
            assert(create_pool(device, &info, NULL, &pool) == VK_SUCCESS && pool);
            assert(reset_pool(device, pool, 0) == VK_SUCCESS);
            assert(reset_pool(device, pool, 1) == VK_SUCCESS);
            destroy_pool(device, pool, NULL);
        }
        info.flags = 0; fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !pool);
        fixture.create_result = VK_SUCCESS;
        if (scenario < 4) {
            if (scenario == 0) fixture.fail_command = 85;
            if (scenario == 1) fixture.corrupt_command = 85;
            if (scenario == 2) fixture.create_result = VK_ERROR_DEVICE_LOST;
            if (scenario == 3) fixture.create_result = VK_NOT_READY;
            assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_DEVICE_LOST && !pool);
            venus_icd_abandon(); continue;
        }
        assert(create_pool(device, &info, NULL, &pool) == VK_SUCCESS && pool);
        destroy_device(device, NULL); assert(lookup(device, "vkResetCommandPool"));
        destroy_pool(NULL, pool, NULL); destroy_pool(device, NULL, NULL);
        destroy_pool((VkDevice)(uintptr_t)1, pool, NULL); destroy_pool(foreign, pool, NULL);
        assert(reset_pool(NULL, pool, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool(device, NULL, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool((VkDevice)(uintptr_t)1, pool, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool(foreign, pool, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool(device, pool, 2) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.pool_reset_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(reset_pool(device, pool, 0) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.pool_reset_result = VK_SUCCESS;
        if (scenario == 4) fixture.fail_command = 87;
        if (scenario == 5) fixture.corrupt_command = 87;
        if (scenario == 6) fixture.pool_reset_result = VK_ERROR_DEVICE_LOST;
        if (scenario == 7) fixture.pool_reset_result = VK_NOT_READY;
        if (scenario >= 4 && scenario <= 7) {
            assert(reset_pool(device, pool, 0) == VK_ERROR_DEVICE_LOST);
            assert(reset_pool(device, pool, 0) == VK_ERROR_DEVICE_LOST);
            assert(create_pool(device, &info, NULL, &pool) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        if (scenario == 8) {
            VkCommandPool pools[504];
            for (unsigned index = 0; index < 504; index++)
                assert(create_pool(device, &info, NULL, &pools[index]) == VK_SUCCESS);
            VkCommandPool exhausted = NULL;
            assert(create_pool(device, &info, NULL, &exhausted) == VK_ERROR_OUT_OF_HOST_MEMORY && !exhausted);
            for (unsigned index = 0; index < 504; index++) destroy_pool(device, pools[index], NULL);
        }
        if (scenario == 9) fixture.fail_command = 86;
        if (scenario == 10) fixture.corrupt_command = 86;
        destroy_pool(device, pool, NULL);
        if (scenario == 9 || scenario == 10) { venus_icd_abandon(); continue; }
        assert(reset_pool(device, pool, 0) == VK_ERROR_INITIALIZATION_FAILED);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void copy_buffer_contract(void) {
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, other_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        pool_info.flags = 0;
        assert(create_pool(device, &pool_info, NULL, &other_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        fixture.command_allocate = &info;
        PFN_vkCmdCopyBuffer copy = (PFN_vkCmdCopyBuffer)lookup(device, "vkCmdCopyBuffer");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
        assert(copy && create_buffer && destroy_buffer && allocate_memory && free_memory && bind);
        VkCommandBuffer buffers[2] = {NULL, NULL};
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4099, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        fixture.buffer_info = &buffer_info;
        VkBuffer source = NULL, destination = NULL, alias = NULL, unbound = NULL;
        VkBuffer foreign_buffer = NULL, wrong_usage = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &source) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &destination) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &alias) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &unbound) == VK_SUCCESS);
        assert(create_buffer(foreign, &buffer_info, NULL, &foreign_buffer) == VK_SUCCESS);
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        assert(create_buffer(device, &buffer_info, NULL, &wrong_usage) == VK_SUCCESS);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 32768, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL, distinct_memory = NULL;
        assert(allocate_memory(device, &memory_info, NULL, &memory) == VK_SUCCESS);
        assert(allocate_memory(device, &memory_info, NULL, &distinct_memory) == VK_SUCCESS);
        assert(bind(device, source, memory, 0) == VK_SUCCESS);
        assert(bind(device, destination, memory, 8192) == VK_SUCCESS);
        assert(bind(device, alias, memory, 0) == VK_SUCCESS);
        assert(bind(device, wrong_usage, memory, 16384) == VK_SUCCESS);
        VkBufferCopy regions[64] = {{.srcOffset = 1, .dstOffset = 3, .size = 7}};
        unsigned submissions = fixture.submissions;
        copy(NULL, source, destination, 1, regions);
        copy((VkCommandBuffer)(uintptr_t)1, source, destination, 1, regions);
        copy(buffers[0], source, destination, 1, regions);
        assert(fixture.submissions == submissions);
        const VkBuffer Invalid[] = {NULL, (VkBuffer)(uintptr_t)1, unbound, foreign_buffer, wrong_usage};
        for (unsigned index = 0; index < sizeof(Invalid) / sizeof(*Invalid); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            copy(buffers[0], Invalid[index], destination, 1, regions);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            copy(buffers[0], source, Invalid[index], 1, regions);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        for (unsigned index = 0; index < 3; index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            copy(buffers[0], source, destination, index == 0 ? 0 : index == 1 ? 65 : 1,
                 index == 2 ? NULL : regions);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        const VkBufferCopy InvalidRegions[] = {
            {0, 0, 0}, {4099, 0, 1}, {0, 4099, 1}, {0, 0, UINT64_MAX},
            {4096, 0, 4}, {0, 4096, 4}, {UINT64_MAX, 0, 1}, {0, UINT64_MAX, 1}};
        for (unsigned index = 0; index < sizeof(InvalidRegions) / sizeof(*InvalidRegions); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            copy(buffers[0], source, destination, 1, &InvalidRegions[index]);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        regions[0] = (VkBufferCopy){0, 128, 16};
        regions[1] = (VkBufferCopy){128, 256, 16};
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        submissions = fixture.submissions;
        copy(buffers[0], source, alias, 2, regions); /* Cross-index overlap. */
        assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        regions[0] = (VkBufferCopy){0, 0, 1};
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        submissions = fixture.submissions;
        copy(buffers[0], source, source, 1, regions);
        assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        if (scenario == 0) fixture.fail_command = 112;
        if (scenario == 1) fixture.corrupt_command = 112;
        regions[0] = (VkBufferCopy){1, 3, 7};
        copy(buffers[0], source, destination, 1, regions);
        if (scenario < 2) {
            assert(end(buffers[0]) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        regions[0] = (VkBufferCopy){0, 16, 16};
        copy(buffers[0], source, source, 1, regions); /* Legal adjacent intervals. */
        for (unsigned index = 0; index < 64; index++)
            regions[index] = (VkBufferCopy){index, 1024 + index, 1};
        copy(buffers[0], source, destination, 64, regions);
        assert(end(buffers[0]) == VK_SUCCESS);
        submissions = fixture.submissions;
        copy(buffers[0], source, destination, 1, regions);
        assert(fixture.submissions == submissions);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        regions[63].srcOffset = 4099;
        submissions = fixture.submissions;
        copy(buffers[0], source, destination, 64, regions);
        assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        regions[63].srcOffset = 63;
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS);
        copy(buffers[1], source, destination, 64, regions);
        if (scenario == 4) assert(reset_pool(device, pool, 0) == VK_SUCCESS);
        if (scenario == 2) {
            destroy_buffer(device, source, NULL); source = NULL;
        } else {
            destroy_buffer(device, destination, NULL); destination = NULL;
        }
        assert(end(buffers[1]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS && end(buffers[1]) == VK_SUCCESS);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer distinct = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &distinct) == VK_SUCCESS);
        assert(bind(device, distinct, distinct_memory, 0) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        regions[0] = (VkBufferCopy){0, 0, 4099};
        copy(buffers[0], alias, distinct, 1, regions); /* Different allocations. */
        assert(end(buffers[0]) == VK_SUCCESS && reset(buffers[0], 0) == VK_SUCCESS);
        destroy_buffer(device, source, NULL); destroy_buffer(device, destination, NULL);
        destroy_buffer(device, alias, NULL); destroy_buffer(device, distinct, NULL);
        destroy_buffer(device, unbound, NULL); destroy_buffer(device, wrong_usage, NULL);
        destroy_buffer(foreign, foreign_buffer, NULL);
        free_memory(device, memory, NULL); free_memory(device, distinct_memory, NULL);
        release(device, pool, 2, buffers);
        destroy_pool(device, pool, NULL); destroy_pool(device, other_pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void update_buffer_contract(void) {
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, other_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        pool_info.flags = 0;
        assert(create_pool(device, &pool_info, NULL, &other_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        fixture.command_allocate = &info;
        PFN_vkCmdUpdateBuffer update = (PFN_vkCmdUpdateBuffer)lookup(device, "vkCmdUpdateBuffer");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
        assert(update && create_buffer && destroy_buffer && allocate_memory && free_memory && bind);
        fixture.requirements_size = 131072;
        VkCommandBuffer buffers[2] = {NULL, NULL};
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 65536, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        fixture.buffer_info = &buffer_info;
        VkBuffer target = NULL, unbound = NULL, foreign_buffer = NULL, source = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &target) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &unbound) == VK_SUCCESS);
        assert(create_buffer(foreign, &buffer_info, NULL, &foreign_buffer) == VK_SUCCESS);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        assert(create_buffer(device, &buffer_info, NULL, &source) == VK_SUCCESS);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 262144, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate_memory(device, &memory_info, NULL, &memory) == VK_SUCCESS);
        assert(bind(device, target, memory, 0) == VK_SUCCESS);
        assert(bind(device, source, memory, 131072) == VK_SUCCESS);
        unsigned char data[65536];
        for (unsigned index = 0; index < sizeof(data); index++) data[index] = (unsigned char)(index * 13);
        fixture.update_data = data;
        unsigned submissions = fixture.submissions;
        update(NULL, target, 0, 4, data);
        update((VkCommandBuffer)(uintptr_t)1, target, 0, 4, data);
        update(buffers[0], target, 0, 4, data);
        assert(fixture.submissions == submissions);
        const VkBuffer Invalid[] = {NULL, (VkBuffer)(uintptr_t)1, unbound, foreign_buffer, source};
        for (unsigned index = 0; index < sizeof(Invalid) / sizeof(*Invalid); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            update(buffers[0], Invalid[index], 0, 4, data);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        const uint64_t Offsets[] = {1, 65536, 0, 0, 0, 65532, UINT64_MAX};
        const uint64_t Sizes[] = {4, 4, 0, 3, 65540, 8, 4};
        for (unsigned index = 0; index < sizeof(Offsets) / sizeof(*Offsets); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            update(buffers[0], target, Offsets[index], Sizes[index], (const void *)(uintptr_t)1);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        submissions = fixture.submissions;
        update(buffers[0], target, 0, 4, NULL);
        assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        if (scenario == 0) fixture.fail_command = 117;
        if (scenario == 1) fixture.corrupt_command = 117;
        update(buffers[0], target, 0, 65536, data);
        if (scenario < 2) {
            assert(end(buffers[0]) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        update(buffers[0], target, 0, 4, data);
        update(buffers[0], target, 4, 8, data);
        assert(end(buffers[0]) == VK_SUCCESS);
        memset(data, 0xa5, sizeof(data)); /* No source pointer survives recording. */
        submissions = fixture.submissions;
        update(buffers[0], target, 0, 4, data);
        assert(fixture.submissions == submissions);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS);
        update(buffers[1], target, 65532, 4, data);
        if (scenario == 3) assert(reset_pool(device, pool, 0) == VK_SUCCESS);
        if (scenario == 4) assert(reset(buffers[1], 0) == VK_SUCCESS);
        destroy_buffer(device, target, NULL);
        assert(end(buffers[1]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS && end(buffers[1]) == VK_SUCCESS);
        destroy_buffer(device, source, NULL); destroy_buffer(device, unbound, NULL);
        destroy_buffer(foreign, foreign_buffer, NULL); free_memory(device, memory, NULL);
        release(device, pool, 2, buffers);
        destroy_pool(device, pool, NULL); destroy_pool(device, other_pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void pipeline_barrier_contract(void) {
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, other_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        pool_info.flags = 0;
        assert(create_pool(device, &pool_info, NULL, &other_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        fixture.command_allocate = &info;
        PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)lookup(device, "vkCmdPipelineBarrier");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
        assert(barrier && create_buffer && destroy_buffer && allocate_memory && free_memory && bind);
        VkCommandBuffer buffers[2] = {NULL, NULL};
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        fixture.buffer_info = &buffer_info;
        VkBuffer target = NULL, unbound = NULL, foreign_buffer = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &target) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &unbound) == VK_SUCCESS);
        assert(create_buffer(foreign, &buffer_info, NULL, &foreign_buffer) == VK_SUCCESS);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 16384, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate_memory(device, &memory_info, NULL, &memory) == VK_SUCCESS);
        assert(bind(device, target, memory, 0) == VK_SUCCESS);
        VkMemoryBarrier memory_barriers[64];
        VkBufferMemoryBarrier buffer_barriers[64];
        for (unsigned index = 0; index < 64; index++) {
            memory_barriers[index] = (VkMemoryBarrier){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
            buffer_barriers[index] = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = target, .offset = 1, .size = 7};
        }
        unsigned submissions = fixture.submissions;
        barrier(NULL, 4096, 4096, 0, 0, NULL, 0, NULL, 0, NULL);
        barrier((VkCommandBuffer)(uintptr_t)1, 4096, 4096, 0, 0, NULL, 0, NULL, 0, NULL);
        barrier(buffers[0], 4096, 4096, 0, 0, NULL, 0, NULL, 0, NULL);
        assert(fixture.submissions == submissions);
        for (unsigned invalid = 0; invalid < 9; invalid++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            barrier(buffers[0], invalid == 0 ? 0 : invalid == 2 ? UINT32_MAX : 4096,
                invalid == 1 ? 0 : invalid == 3 ? UINT32_MAX : 4096,
                invalid == 4 ? 2 : 0, invalid == 5 ? 65 : invalid == 7 ? 1 : 0,
                invalid == 7 ? NULL : memory_barriers,
                invalid == 6 ? 65 : invalid == 8 ? 1 : 0, invalid == 8 ? NULL : buffer_barriers,
                0, NULL);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        submissions = fixture.submissions;
        barrier(buffers[0], 4096, 4096, 0, 0, NULL, 0, NULL, 65, (const void *)(uintptr_t)1);
        assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        for (unsigned invalid = 0; invalid < 4; invalid++) {
            VkMemoryBarrier saved = memory_barriers[63];
            if (invalid == 0) memory_barriers[63].sType = 0;
            if (invalid == 1) memory_barriers[63].pNext = &memory_barriers;
            if (invalid == 2) memory_barriers[63].srcAccessMask = UINT32_MAX;
            if (invalid == 3) memory_barriers[63].dstAccessMask = UINT32_MAX;
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            barrier(buffers[0], 4096, 4096, 0, 64, memory_barriers, 0, NULL, 0, NULL);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
            memory_barriers[63] = saved;
        }
        for (unsigned invalid = 0; invalid < 16; invalid++) {
            VkBufferMemoryBarrier saved = buffer_barriers[63];
            if (invalid == 0) buffer_barriers[63].sType = 0;
            if (invalid == 1) buffer_barriers[63].pNext = &buffer_barriers;
            if (invalid == 2) buffer_barriers[63].srcAccessMask = UINT32_MAX;
            if (invalid == 3) buffer_barriers[63].dstAccessMask = UINT32_MAX;
            if (invalid == 4) buffer_barriers[63].buffer = NULL;
            if (invalid == 5) buffer_barriers[63].buffer = (VkBuffer)(uintptr_t)1;
            if (invalid == 6) buffer_barriers[63].buffer = foreign_buffer;
            if (invalid == 7) buffer_barriers[63].buffer = unbound;
            if (invalid == 8) buffer_barriers[63].srcQueueFamilyIndex = 0;
            if (invalid == 9) buffer_barriers[63].dstQueueFamilyIndex = 0;
            if (invalid == 10) buffer_barriers[63].srcQueueFamilyIndex = buffer_barriers[63].dstQueueFamilyIndex = 99;
            if (invalid == 11) { buffer_barriers[63].srcQueueFamilyIndex = 0; buffer_barriers[63].dstQueueFamilyIndex = 99; }
            if (invalid == 12) buffer_barriers[63].offset = 4096;
            if (invalid == 13) buffer_barriers[63].size = 0;
            if (invalid == 14) buffer_barriers[63].size = UINT64_MAX - 1;
            if (invalid == 15) { buffer_barriers[63].offset = 4093; buffer_barriers[63].size = 4; }
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            barrier(buffers[0], 4096, 4096, 0, 0, NULL, 64, buffer_barriers, 0, NULL);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
            buffer_barriers[63] = saved;
        }
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        if (scenario == 0) fixture.fail_command = 126;
        if (scenario == 1) fixture.corrupt_command = 126;
        barrier(buffers[0], 4096, 4096, 1, 64, memory_barriers, 64, buffer_barriers, 0, NULL);
        if (scenario < 2) { assert(end(buffers[0]) == VK_ERROR_DEVICE_LOST); venus_icd_abandon(); continue; }
        barrier(buffers[0], 4096, 4096, 0, 0, (const void *)(uintptr_t)1,
                0, (const void *)(uintptr_t)1, 0, (const void *)(uintptr_t)1);
        buffer_barriers[0].srcQueueFamilyIndex = buffer_barriers[0].dstQueueFamilyIndex = 0;
        buffer_barriers[0].offset = 0; buffer_barriers[0].size = VK_WHOLE_SIZE;
        memory_barriers[0].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer_barriers[0].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier(buffers[0], 4096, 16384, 0, 1, memory_barriers, 1, buffer_barriers, 0, NULL);
        assert(end(buffers[0]) == VK_SUCCESS);
        submissions = fixture.submissions;
        barrier(buffers[0], 4096, 4096, 0, 0, NULL, 0, NULL, 0, NULL);
        assert(fixture.submissions == submissions);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS);
        buffer_barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier(buffers[1], 4096, 4096, 0, 0, NULL, 1, buffer_barriers, 0, NULL);
        if (scenario == 3) assert(reset_pool(device, pool, 0) == VK_SUCCESS);
        if (scenario == 4) assert(reset(buffers[1], 0) == VK_SUCCESS);
        destroy_buffer(device, target, NULL);
        assert(end(buffers[1]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS && end(buffers[1]) == VK_SUCCESS);
        destroy_buffer(device, unbound, NULL); destroy_buffer(foreign, foreign_buffer, NULL);
        free_memory(device, memory, NULL); release(device, pool, 2, buffers);
        destroy_pool(device, pool, NULL); destroy_pool(device, other_pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void fill_buffer_contract(void) {
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, other_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        pool_info.flags = 0;
        assert(create_pool(device, &pool_info, NULL, &other_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        fixture.command_allocate = &info;
        PFN_vkCmdFillBuffer fill = (PFN_vkCmdFillBuffer)lookup(device, "vkCmdFillBuffer");
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)lookup(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)lookup(device, "vkDestroyBuffer");
        PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)lookup(device, "vkAllocateMemory");
        PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)lookup(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)lookup(device, "vkBindBufferMemory");
        assert(fill && create_buffer && destroy_buffer && allocate_memory && free_memory && bind);
        VkCommandBuffer buffers[2] = {NULL, NULL};
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4099, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        fixture.buffer_info = &buffer_info;
        VkBuffer target = NULL, unbound = NULL, foreign_buffer = NULL, source = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &target) == VK_SUCCESS);
        assert(create_buffer(device, &buffer_info, NULL, &unbound) == VK_SUCCESS);
        assert(create_buffer(foreign, &buffer_info, NULL, &foreign_buffer) == VK_SUCCESS);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        assert(create_buffer(device, &buffer_info, NULL, &source) == VK_SUCCESS);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 16384, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate_memory(device, &memory_info, NULL, &memory) == VK_SUCCESS);
        assert(bind(device, target, memory, 0) == VK_SUCCESS);
        assert(bind(device, source, memory, 8192) == VK_SUCCESS);
        unsigned submissions = fixture.submissions;
        fill(NULL, target, 0, 4, 1);
        fill((VkCommandBuffer)(uintptr_t)1, target, 0, 4, 1);
        fill(buffers[0], target, 0, 4, 1); /* Initial preserves state. */
        assert(fixture.submissions == submissions);
        const VkBuffer Invalid[] = {NULL, (VkBuffer)(uintptr_t)1, unbound, foreign_buffer, source};
        for (unsigned index = 0; index < sizeof(Invalid) / sizeof(*Invalid); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            fill(buffers[0], Invalid[index], 0, 4, 1);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        const uint64_t Offsets[] = {1, 4100, 0, 0, 4, UINT64_MAX};
        const uint64_t Sizes[] = {4, 4, 0, 3, UINT64_MAX - 3, 4};
        for (unsigned index = 0; index < sizeof(Offsets) / sizeof(*Offsets); index++) {
            assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            submissions = fixture.submissions;
            fill(buffers[0], target, Offsets[index], Sizes[index], 1);
            assert(fixture.submissions == submissions && end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        if (scenario == 0) fixture.fail_command = 118;
        if (scenario == 1) fixture.corrupt_command = 118;
        fill(buffers[0], target, 0, 4096, 0x12345678);
        if (scenario < 2) {
            assert(end(buffers[0]) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        fill(buffers[0], target, 4, VK_WHOLE_SIZE, 0);
        fill(buffers[0], target, 4096, VK_WHOLE_SIZE, UINT32_MAX);
        assert(end(buffers[0]) == VK_SUCCESS);
        submissions = fixture.submissions;
        fill(buffers[0], target, 0, 4, 1); /* Executable preserves state. */
        assert(fixture.submissions == submissions);
        if (scenario == 2) assert(reset(buffers[0], 0) == VK_SUCCESS);
        if (scenario == 3) assert(reset_pool(device, pool, 0) == VK_SUCCESS);
        if (scenario == 4) {
            assert(begin(buffers[1], &begin_info) == VK_SUCCESS);
            fill(buffers[1], target, 0, 4, 1);
        }
        destroy_buffer(device, target, NULL);
        if (scenario == 4) {
            assert(end(buffers[1]) == VK_ERROR_INITIALIZATION_FAILED);
            assert(end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        }
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        assert(create_buffer(device, &buffer_info, NULL, &target) == VK_SUCCESS);
        assert(bind(device, target, memory, 0) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        assert(begin(buffers[1], &begin_info) == VK_SUCCESS && end(buffers[1]) == VK_SUCCESS);
        destroy_buffer(device, target, NULL); /* Cleared refs must not invalidate. */
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        destroy_buffer(device, source, NULL); destroy_buffer(device, unbound, NULL);
        destroy_buffer(foreign, foreign_buffer, NULL); free_memory(device, memory, NULL);
        release(device, pool, 2, buffers);
        destroy_pool(device, pool, NULL); destroy_pool(device, other_pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void queue_submit_contract(void) {
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priorities[2] = {1,1};
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 2, .pQueuePriorities = priorities};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)lookup(device, "vkQueueSubmit");
        PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)lookup(device, "vkGetDeviceQueue");
        PFN_vkQueueWaitIdle idle = (PFN_vkQueueWaitIdle)lookup(device, "vkQueueWaitIdle");
        PFN_vkDeviceWaitIdle device_idle = (PFN_vkDeviceWaitIdle)lookup(device, "vkDeviceWaitIdle");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)lookup(device, "vkCreateSemaphore");
        PFN_vkDestroySemaphore destroy_semaphore = (PFN_vkDestroySemaphore)lookup(device, "vkDestroySemaphore");
        PFN_vkCreateFence create_fence = (PFN_vkCreateFence)lookup(device, "vkCreateFence");
        PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)lookup(device, "vkDestroyFence");
        PFN_vkGetFenceStatus status = (PFN_vkGetFenceStatus)lookup(device, "vkGetFenceStatus");
        PFN_vkResetFences reset_fences = (PFN_vkResetFences)lookup(device, "vkResetFences");
        PFN_vkWaitForFences wait = (PFN_vkWaitForFences)lookup(device, "vkWaitForFences");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        assert(submit && get_queue && idle && device_idle);
        VkQueue queues[2] = {NULL,NULL};
        get_queue(device, 0, 0, &queues[0]); get_queue(device, 0, 1, &queues[1]);
        assert(queues[0] && queues[1]);
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, foreign_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        assert(create_pool(foreign, &pool_info, NULL, &foreign_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .commandBufferCount = 2};
        fixture.command_allocate = &allocation;
        VkCommandBuffer commands[2] = {NULL,NULL}, foreign_command = NULL, secondary = NULL;
        assert(allocate(device, &allocation, commands) == VK_SUCCESS);
        allocation.commandPool = foreign_pool; allocation.commandBufferCount = 1;
        assert(allocate(foreign, &allocation, &foreign_command) == VK_SUCCESS);
        allocation.commandPool = pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        assert(allocate(device, &allocation, &secondary) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        for (unsigned index = 0; index < 2; index++)
            assert(begin(commands[index], &begin_info) == VK_SUCCESS && end(commands[index]) == VK_SUCCESS);
        assert(begin(foreign_command, &begin_info) == VK_SUCCESS && end(foreign_command) == VK_SUCCESS);
        VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphores[2] = {NULL,NULL}, foreign_semaphore = NULL;
        for (unsigned index = 0; index < 2; index++)
            assert(create_semaphore(device, &semaphore_info, NULL, &semaphores[index]) == VK_SUCCESS);
        assert(create_semaphore(foreign, &semaphore_info, NULL, &foreign_semaphore) == VK_SUCCESS);
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fences[2] = {NULL,NULL}, foreign_fence = NULL;
        for (unsigned index = 0; index < 2; index++)
            assert(create_fence(device, &fence_info, NULL, &fences[index]) == VK_SUCCESS);
        assert(create_fence(foreign, &fence_info, NULL, &foreign_fence) == VK_SUCCESS);
        VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = commands};
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        unsigned before = fixture.submissions;
        assert(submit(NULL,0,NULL,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit((VkQueue)(uintptr_t)1,0,NULL,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit(queues[0],1,NULL,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit(queues[0],17,(const void *)(uintptr_t)1,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        info.sType = 0; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; info.pNext = &info;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED); info.pNext = NULL;
        info.waitSemaphoreCount = 65; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        info.waitSemaphoreCount = 0; info.signalSemaphoreCount = 65;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY); info.signalSemaphoreCount = 0;
        info.commandBufferCount = 65; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        info.commandBufferCount = 1; info.pCommandBuffers = NULL;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        VkCommandBuffer invalid_command = NULL; info.pCommandBuffers = &invalid_command;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        invalid_command = (VkCommandBuffer)(uintptr_t)1;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pCommandBuffers = &foreign_command; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pCommandBuffers = &secondary; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pCommandBuffers = commands; info.waitSemaphoreCount = 1;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pWaitSemaphores = semaphores;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pWaitDstStageMask = &stage; stage = 0;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        stage = 0x20000; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT; info.pWaitSemaphores = &foreign_semaphore;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        VkSemaphore invalid_semaphore = NULL; info.pWaitSemaphores = &invalid_semaphore;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        invalid_semaphore = (VkSemaphore)(uintptr_t)1;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.waitSemaphoreCount = 0; info.signalSemaphoreCount = 1;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pSignalSemaphores = &invalid_semaphore;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        invalid_semaphore = NULL; assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.pSignalSemaphores = &foreign_semaphore;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        info.signalSemaphoreCount = 0;
        assert(submit(queues[0],1,&info,foreign_fence) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit(queues[0],1,&info,(VkFence)(uintptr_t)1) == VK_ERROR_INITIALIZATION_FAILED);
        VkCommandBuffer duplicates[2] = {commands[0],commands[0]};
        info.commandBufferCount = 2; info.pCommandBuffers = duplicates;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(fixture.submissions == before);
        info.commandBufferCount = 1; info.pCommandBuffers = commands;
        fixture.submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        assert(reset(commands[0],0) == VK_SUCCESS);
        assert(begin(commands[0],&begin_info) == VK_SUCCESS && end(commands[0]) == VK_SUCCESS);
        fixture.submit_result = VK_SUCCESS;
        if (scenario < 4) {
            if (scenario == 0) fixture.fail_command = 18;
            if (scenario == 1) fixture.corrupt_command = 18;
            if (scenario == 2) fixture.submit_result = VK_ERROR_DEVICE_LOST;
            if (scenario == 3) fixture.submit_result = VK_NOT_READY;
            assert(submit(queues[0],1,&info,NULL) == VK_ERROR_DEVICE_LOST);
            assert(submit(queues[0],1,&info,NULL) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        info.waitSemaphoreCount = 1; info.pWaitSemaphores = semaphores; info.pWaitDstStageMask = &stage;
        info.signalSemaphoreCount = 1; info.pSignalSemaphores = semaphores + 1;
        assert(submit(queues[0],1,&info,fences[0]) == VK_SUCCESS);
        uint64_t first_id = fixture.submit_fence;
        assert(first_id && first_id < 4096);
        before = fixture.submissions;
        assert(reset(commands[0],0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool(device,pool,0) == VK_ERROR_INITIALIZATION_FAILED);
        release(device,pool,1,commands); destroy_pool(device,pool,NULL);
        destroy_semaphore(device,semaphores[0],NULL); destroy_semaphore(device,semaphores[1],NULL);
        destroy_fence(device,fences[0],NULL);
        assert(reset_fences(device,1,fences) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(submit(queues[0],0,NULL,fences[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(fixture.submissions == before);
        assert(status(device,fences[0]) == VK_NOT_READY);
        assert(reset(commands[0],0) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.fence_ready[first_id] = 1;
        assert(status(device,fences[0]) == VK_SUCCESS);
        assert(reset(commands[0],0) == VK_SUCCESS);
        /* A natively signaled fence cannot be submitted until reset. */
        assert(submit(queues[0],0,NULL,fences[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_fences(device,1,fences) == VK_SUCCESS);
        info.waitSemaphoreCount = 0; info.signalSemaphoreCount = 0;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        assert(begin(commands[0],&begin_info) == VK_SUCCESS && end(commands[0]) == VK_SUCCESS);
        assert(submit(queues[0],1,&info,NULL) == VK_SUCCESS);
        assert(idle(queues[0]) == VK_SUCCESS);
        assert(submit(queues[0],1,&info,NULL) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        assert(begin(commands[0],&begin_info) == VK_SUCCESS && end(commands[0]) == VK_SUCCESS);
        info.commandBufferCount = 2; info.pCommandBuffers = duplicates;
        assert(submit(queues[0],1,&info,NULL) == VK_SUCCESS);
        assert(submit(queues[1],1,&info,NULL) == VK_SUCCESS);
        assert(idle(queues[0]) == VK_SUCCESS && reset(commands[0],0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(idle(queues[1]) == VK_SUCCESS && reset(commands[0],0) == VK_SUCCESS);
        begin_info.flags = 0;
        assert(begin(commands[0],&begin_info) == VK_SUCCESS && end(commands[0]) == VK_SUCCESS);
        info.commandBufferCount = 1; info.pCommandBuffers = commands;
        assert(submit(queues[0],1,&info,fences[0]) == VK_SUCCESS); first_id = fixture.submit_fence;
        info.pCommandBuffers = commands + 1;
        assert(submit(queues[1],1,&info,fences[1]) == VK_SUCCESS);
        uint64_t second_id = fixture.submit_fence;
        assert(wait(device,2,fences,VK_TRUE,0) == VK_TIMEOUT);
        fixture.fence_ready[first_id] = 1;
        assert(wait(device,2,fences,VK_FALSE,0) == VK_SUCCESS);
        assert(reset_fences(device,1,fences) == VK_SUCCESS);
        assert(reset_fences(device,1,fences+1) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.fence_ready[second_id] = 1;
        /* Re-signal the first already completed fence for native wait-all. */
        fixture.fence_ready[first_id] = 1;
        assert(wait(device,2,fences,VK_TRUE,0) == VK_SUCCESS);
        assert(reset(commands[1],0) == VK_SUCCESS);
        for (unsigned index = 0; index < 128; index++) assert(submit(queues[0],0,(const void *)(uintptr_t)1,NULL) == VK_SUCCESS);
        assert(submit(queues[0],0,NULL,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        before = fixture.submissions; destroy_device(device,NULL); assert(fixture.submissions == before);
        assert(device_idle(device) == VK_SUCCESS);
        VkSubmitInfo empty[16];
        for (unsigned index = 0; index < 16; index++) empty[index] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pWaitSemaphores = (const void *)(uintptr_t)1, .pWaitDstStageMask = (const void *)(uintptr_t)1,
            .pCommandBuffers = (const void *)(uintptr_t)1, .pSignalSemaphores = (const void *)(uintptr_t)1};
        assert(submit(queues[0],16,empty,NULL) == VK_SUCCESS && idle(queues[0]) == VK_SUCCESS);
        if (scenario == 7) {
            VkSemaphore bounded_semaphores[64] = {0};
            VkCommandBuffer bounded_commands[64];
            VkPipelineStageFlags bounded_stages[64];
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            assert(begin(commands[0],&begin_info) == VK_SUCCESS && end(commands[0]) == VK_SUCCESS);
            for (unsigned index = 0; index < 64; index++) {
                assert(create_semaphore(device,&semaphore_info,NULL,&bounded_semaphores[index]) == VK_SUCCESS);
                bounded_commands[index] = commands[0]; bounded_stages[index] = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            VkSubmitInfo bounded = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .signalSemaphoreCount = 64, .pSignalSemaphores = bounded_semaphores};
            assert(submit(queues[0],1,&bounded,NULL) == VK_SUCCESS);
            bounded.waitSemaphoreCount = 64; bounded.pWaitSemaphores = bounded_semaphores;
            bounded.pWaitDstStageMask = bounded_stages; bounded.commandBufferCount = 64;
            bounded.pCommandBuffers = bounded_commands;
            assert(submit(queues[0],1,&bounded,NULL) == VK_SUCCESS);
            VkSubmitInfo excessive[2] = {bounded,bounded};
            before = fixture.submissions;
            assert(submit(queues[0],2,excessive,NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
            assert(fixture.submissions == before && idle(queues[0]) == VK_SUCCESS);
            assert(reset(commands[0],0) == VK_SUCCESS);
            for (unsigned index = 0; index < 64; index++) destroy_semaphore(device,bounded_semaphores[index],NULL);
        }
        if (scenario == 4 || scenario == 5) {
            fixture.fence_override = 1;
            fixture.fence_result = scenario == 4 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_DEVICE_LOST;
            assert(submit(queues[0],0,NULL,fences[1]) == fixture.fence_result);
            fixture.fence_override = 0;
            if (scenario == 5) { venus_icd_abandon(); continue; }
        }
        release(device,pool,2,commands); release(device,pool,1,&secondary);
        release(foreign,foreign_pool,1,&foreign_command);
        destroy_pool(device,pool,NULL); destroy_pool(foreign,foreign_pool,NULL);
        for (unsigned index = 0; index < 2; index++) {
            destroy_semaphore(device,semaphores[index],NULL); destroy_fence(device,fences[index],NULL);
        }
        destroy_semaphore(foreign,foreign_semaphore,NULL); destroy_fence(foreign,foreign_fence,NULL);
        destroy_device(device,NULL); destroy_device(foreign,NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}

static void semaphore_contract(void) {
    for (unsigned scenario = 0; scenario < 12; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)lookup(device, "vkCreateSemaphore");
        PFN_vkDestroySemaphore release = (PFN_vkDestroySemaphore)lookup(device, "vkDestroySemaphore");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        assert(create_semaphore && release && destroy_device);
        VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = (VkSemaphore)(uintptr_t)1;
        unsigned submissions = fixture.submissions;
        assert(create_semaphore(device, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_semaphore(NULL, &info, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED && !semaphore);
        assert(create_semaphore((VkDevice)(uintptr_t)1, &info, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED);
        assert(create_semaphore(device, NULL, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = 0;
        assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED);
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; info.pNext = &info;
        assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED);
        info.pNext = NULL; info.flags = 1;
        assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_INITIALIZATION_FAILED);
        info.flags = 0;
        assert(fixture.submissions == submissions);
        fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !semaphore);
        fixture.semaphore_fault = 4;
        assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !semaphore);
        fixture.semaphore_fault = 0; fixture.create_result = VK_SUCCESS;
        if (scenario < 7) {
            if (scenario == 0) fixture.fail_command = 40;
            if (scenario == 1) fixture.corrupt_command = 40;
            if (scenario >= 2 && scenario <= 4) fixture.semaphore_fault = scenario - 1;
            if (scenario == 5) fixture.create_result = VK_ERROR_DEVICE_LOST;
            if (scenario == 6) fixture.create_result = VK_NOT_READY;
            assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_DEVICE_LOST && !semaphore);
            assert(create_semaphore(device, &info, NULL, &semaphore) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        assert(create_semaphore(device, &info, (const void *)(uintptr_t)1, &semaphore) == VK_SUCCESS && semaphore);
        submissions = fixture.submissions;
        release(NULL, semaphore, NULL); release((VkDevice)(uintptr_t)1, semaphore, NULL);
        release(foreign, semaphore, NULL); release(device, NULL, NULL);
        release(device, (VkSemaphore)(uintptr_t)1, NULL); release(device, (VkSemaphore)device, NULL);
        destroy_device(device, NULL); /* Live child preserves parent. */
        assert(fixture.submissions == submissions);
        if (scenario == 7) fixture.fail_command = 41;
        if (scenario == 8) fixture.corrupt_command = 41;
        if (scenario == 9) {
            VkSemaphore extra[504];
            for (unsigned index = 0; index < 504; index++)
                assert(create_semaphore(device, &info, NULL, &extra[index]) == VK_SUCCESS);
            VkSemaphore exhausted = NULL;
            submissions = fixture.submissions;
            assert(create_semaphore(device, &info, NULL, &exhausted) == VK_ERROR_OUT_OF_HOST_MEMORY && !exhausted);
            assert(fixture.submissions == submissions);
            for (unsigned index = 0; index < 504; index++) release(device, extra[index], NULL);
        }
        release(device, semaphore, (const void *)(uintptr_t)1);
        if (scenario == 7 || scenario == 8) { venus_icd_abandon(); continue; }
        submissions = fixture.submissions;
        release(device, semaphore, NULL);
        assert(fixture.submissions == submissions);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void command_buffer_contract(void) {
    for (unsigned scenario = 0; scenario < 20; scenario++) {
        fixture_t fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        VkInstance instance = create();
        uint32_t count = 2;
        VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
            "vkEnumeratePhysicalDevices"))(instance, &count, physical) == VK_SUCCESS);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        fixture.device_info = &device_info;
        VkDevice device = NULL, foreign = NULL;
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup_external(instance, "vkCreateDevice");
        assert(create_device(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
        assert(create_device(physical[0], &device_info, NULL, &foreign) == VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup = (PFN_vkGetDeviceProcAddr)lookup_external(instance, "vkGetDeviceProcAddr");
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)lookup(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)lookup(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool = (PFN_vkResetCommandPool)lookup(device, "vkResetCommandPool");
        PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)lookup(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)lookup(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)lookup(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)lookup(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)lookup(device, "vkResetCommandBuffer");
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice");
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL, other_pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
        pool_info.flags = 0;
        assert(create_pool(device, &pool_info, NULL, &other_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        fixture.command_allocate = &info;
        VkCommandBuffer buffers[64] = {0};
        assert(allocate(device, &info, NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate(device, NULL, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate(NULL, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocate((VkDevice)(uintptr_t)1, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.commandBufferCount = 0;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.commandBufferCount = 65;
        buffers[0] = (VkCommandBuffer)(uintptr_t)1;
        assert(allocate(device, &info, buffers) == VK_ERROR_OUT_OF_HOST_MEMORY && buffers[0]);
        info.commandBufferCount = 2; info.sType = 0;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED && !buffers[0]);
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; info.pNext = &info;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.pNext = NULL; info.level = 99;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; info.commandPool = NULL;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.commandPool = (VkCommandPool)(uintptr_t)1;
        assert(allocate(device, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        info.commandPool = pool;
        assert(allocate(foreign, &info, buffers) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(allocate(device, &info, buffers) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !buffers[0] && !buffers[1]);
        fixture.command_array_fault = 4;
        assert(allocate(device, &info, buffers) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !buffers[0] && !buffers[1]);
        fixture.command_array_fault = 0; fixture.create_result = VK_SUCCESS;
        if (scenario < 7) {
            if (scenario == 0) fixture.fail_command = 88;
            if (scenario == 1) fixture.corrupt_command = 88;
            if (scenario >= 2 && scenario <= 4) fixture.command_array_fault = scenario - 1;
            if (scenario == 5) fixture.create_result = VK_ERROR_DEVICE_LOST;
            if (scenario == 6) fixture.create_result = VK_NOT_READY;
            assert(allocate(device, &info, buffers) == VK_ERROR_DEVICE_LOST && !buffers[0] && !buffers[1]);
            assert(allocate(device, &info, buffers) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        assert(allocate(device, &info, buffers) == VK_SUCCESS && buffers[0] && buffers[1]);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        assert(end(NULL) == VK_ERROR_INITIALIZATION_FAILED);
        assert(end((VkCommandBuffer)(uintptr_t)1) == VK_ERROR_INITIALIZATION_FAILED);
        assert(end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(NULL, &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin((VkCommandBuffer)(uintptr_t)1, &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[0], NULL) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.sType = 0;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; begin_info.pNext = &begin_info;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.pNext = NULL; begin_info.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.flags = 5;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        begin_info.flags = 0;
        begin_info.pInheritanceInfo = (const VkCommandBufferInheritanceInfo *)(uintptr_t)1;
        fixture.command_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.command_result = VK_SUCCESS;
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        assert(end(buffers[0]) == VK_SUCCESS);
        assert(reset(buffers[0], 0) == VK_SUCCESS);
        if (scenario >= 7 && scenario <= 12) {
            unsigned command = 90 + (scenario - 7) / 2;
            if (command != 90) assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
            if (scenario % 2) fixture.fail_command = command;
            else fixture.corrupt_command = command;
            VkResult result = command == 90 ? begin(buffers[0], &begin_info) :
                              command == 91 ? end(buffers[0]) : reset(buffers[0], 0);
            assert(result == VK_ERROR_DEVICE_LOST);
            assert(begin(buffers[0], &begin_info) == VK_ERROR_DEVICE_LOST);
            assert(end(buffers[0]) == VK_ERROR_DEVICE_LOST);
            assert(reset(buffers[0], 0) == VK_ERROR_DEVICE_LOST);
            venus_icd_abandon(); continue;
        }
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.command_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(end(buffers[0]) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        assert(end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.command_result = VK_SUCCESS;
        assert(reset(buffers[0], 1) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        assert(reset(NULL, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset((VkCommandBuffer)(uintptr_t)1, 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset(buffers[0], 2) == VK_ERROR_INITIALIZATION_FAILED);
        fixture.command_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(reset(buffers[0], 0) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.command_result = VK_SUCCESS;
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS);
        assert(reset_pool(device, pool, 0) == VK_SUCCESS);
        assert(end(buffers[0]) == VK_ERROR_INITIALIZATION_FAILED);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        unsigned submissions = fixture.submissions;
        release(NULL, pool, 2, buffers); release((VkDevice)(uintptr_t)1, pool, 2, buffers);
        release(foreign, pool, 2, buffers); release(device, NULL, 2, buffers);
        release(device, (VkCommandPool)(uintptr_t)1, 2, buffers);
        release(device, other_pool, 2, buffers); release(device, pool, 65, buffers);
        release(device, pool, 0, NULL); release(device, pool, 2, NULL);
        VkCommandBuffer duplicate[2] = {buffers[0], buffers[0]};
        release(device, pool, 2, duplicate);
        duplicate[1] = NULL; release(device, pool, 2, duplicate);
        duplicate[1] = (VkCommandBuffer)(uintptr_t)1; release(device, pool, 2, duplicate);
        assert(fixture.submissions == submissions);
        if (scenario == 13) fixture.fail_command = 89;
        if (scenario == 14) fixture.corrupt_command = 89;
        release(device, pool, 2, buffers);
        if (scenario == 13 || scenario == 14) { venus_icd_abandon(); continue; }
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        release(device, pool, 2, buffers);
        if (scenario == 15) {
            /* Fixed command metadata quota: reset scrubs bindings without refunding live owners. */
            VkCommandBuffer batch[64], extra[2] = {0};
            info.commandBufferCount = 64;
            assert(allocate(device, &info, batch) == VK_SUCCESS);
            info.commandBufferCount = 1;
            submissions = fixture.submissions;
            assert(allocate(device, &info, extra) == VK_ERROR_OUT_OF_HOST_MEMORY && !extra[0]);
            assert(fixture.submissions == submissions);
            assert(reset_pool(device, pool, 0) == VK_SUCCESS);
            assert(reset(batch[0], 0) == VK_SUCCESS);
            submissions = fixture.submissions;
            assert(allocate(device, &info, extra) == VK_ERROR_OUT_OF_HOST_MEMORY && !extra[0]);
            assert(fixture.submissions == submissions);
            release(device, pool, 1, batch);
            info.commandBufferCount = 2;
            assert(allocate(device, &info, extra) == VK_ERROR_OUT_OF_HOST_MEMORY && !extra[0] && !extra[1]);
            /* Partial metadata reservations from the failed two-owner allocation are refunded. */
            info.commandBufferCount = 1;
            assert(allocate(device, &info, extra) == VK_SUCCESS && extra[0]);
            release(device, pool, 1, extra);
            release(device, pool, 63, batch + 1);
            info.commandBufferCount = 64;
            assert(allocate(device, &info, batch) == VK_SUCCESS);
            release(device, pool, 64, batch);
        }
        info.commandBufferCount = 2; info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        begin_info.pInheritanceInfo = NULL;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        begin_info.pInheritanceInfo = &inheritance;
        inheritance.sType = 0;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO; inheritance.pNext = &inheritance;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.pNext = NULL; inheritance.renderPass = (VkRenderPass)(uintptr_t)1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.renderPass = NULL; inheritance.framebuffer = (VkFramebuffer)(uintptr_t)1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.framebuffer = NULL; inheritance.subpass = 1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.subpass = 0; inheritance.occlusionQueryEnable = 1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.occlusionQueryEnable = 0; inheritance.queryFlags = 1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.queryFlags = 0; inheritance.pipelineStatistics = 1;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        inheritance.pipelineStatistics = 0; begin_info.flags = 5;
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        destroy_pool(device, pool, NULL); /* Implicitly retires both child buffers. */
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        info.commandPool = other_pool; info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; info.commandBufferCount = 1;
        assert(allocate(device, &info, buffers) == VK_SUCCESS);
        begin_info.flags = 0;
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset(buffers[0], 0) == VK_ERROR_INITIALIZATION_FAILED);
        assert(reset_pool(device, other_pool, 0) == VK_SUCCESS);
        fixture.command_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fixture.command_result = VK_SUCCESS;
        submissions = fixture.submissions;
        assert(begin(buffers[0], &begin_info) == VK_ERROR_INITIALIZATION_FAILED);
        assert(fixture.submissions == submissions);
        assert(reset_pool(device, other_pool, 0) == VK_SUCCESS);
        assert(begin(buffers[0], &begin_info) == VK_SUCCESS && end(buffers[0]) == VK_SUCCESS);
        destroy_pool(device, other_pool, NULL);
        destroy_device(device, NULL); destroy_device(foreign, NULL); destroy(instance);
        assert(venus_icd_unbind() == RingOk);
    }
}
static void version_contract(void) {
  fixture_t fixture = fresh();
  assert(venus_icd_bind(exchange, &fixture) == RingOk);
  VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
  VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &application};
  const uint32_t Rejected[] = {VK_API_VERSION_1_1, VK_API_VERSION_1_3,
                               VK_MAKE_API_VERSION(1, 1, 0, 0),
                               VK_MAKE_API_VERSION(0, 2, 0, 0), 1};
  for (unsigned index = 0; index < sizeof(Rejected) / sizeof(*Rejected);
       index++) {
    application.apiVersion = Rejected[index];
    VkInstance instance = (VkInstance)(uintptr_t)1;
    assert(create_function()(&info, NULL, &instance) ==
           VK_ERROR_INCOMPATIBLE_DRIVER);
    assert(!instance && !fixture.submissions);
  }
  const uint32_t Accepted[] = {0, VK_API_VERSION_1_0,
                               VK_MAKE_API_VERSION(0, 1, 0, 4095)};
  for (unsigned index = 0; index < sizeof(Accepted) / sizeof(*Accepted);
       index++) {
    application.apiVersion = Accepted[index];
    VkInstance instance = NULL;
    assert(create_function()(&info, NULL, &instance) == VK_SUCCESS && instance);
    destroy(instance);
  }
  assert(venus_icd_unbind() == RingOk);
  const uint32_t InvalidHost[] = {0, VK_MAKE_API_VERSION(1, 1, 0, 0),
                                  VK_MAKE_API_VERSION(0, 2, 0, 0)};
  for (unsigned index = 0; index < sizeof(InvalidHost) / sizeof(*InvalidHost);
       index++) {
    fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkInstance instance = create();
    VkPhysicalDevice devices[2];
    uint32_t count = 2;
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)lookup_external(
            instance, "vkEnumeratePhysicalDevices");
    assert(enumerate(instance, &count, devices) == VK_SUCCESS);
    fixture.properties_override = 1;
    fixture.properties_version = InvalidHost[index];
    VkPhysicalDeviceProperties output, saved;
    memset(&output, 0xa5, sizeof(output));
    memcpy(&saved, &output, sizeof(saved));
    ((PFN_vkGetPhysicalDeviceProperties)lookup_external(
        instance, "vkGetPhysicalDeviceProperties"))(devices[0], &output);
    assert(!memcmp(&saved, &output, sizeof(saved)));
    assert(enumerate(instance, &count, devices) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
  }
}
static void failures(void) {
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = (VkInstance)(uintptr_t)1;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED &&
           !instance);
    assert(create_function()(&info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    fixture_t fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    info.flags = 1;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED &&
           !instance);
    info.flags = 0;
    assert(create_function()(NULL, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    fixture.create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_OUT_OF_HOST_MEMORY && !instance);
    assert(venus_icd_unbind() == RingOk);
    const uint32_t Commands[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 33};
    for (unsigned item = 0; item < sizeof(Commands) / sizeof(*Commands); item++) {
        uint32_t command = Commands[item];
        fixture = fresh();
        assert(venus_icd_bind(exchange, &fixture) == RingOk);
        if (command == 0) {
            fixture.corrupt_command = 0;
            assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST && !instance);
        } else {
            instance = create();
            fixture.corrupt_command = command;
            if (command == 1)
                destroy(instance);
            else {
                PFN_vkEnumeratePhysicalDevices enumerate =
                    (PFN_vkEnumeratePhysicalDevices)lookup_external(instance,
                                                                    "vkEnumeratePhysicalDevices");
                uint32_t count = 2;
                VkPhysicalDevice devices[2];
                if (command == 2)
                    assert(enumerate(instance, &count, devices) == VK_ERROR_DEVICE_LOST);
                else {
                    assert(enumerate(instance, &count, devices) == VK_SUCCESS);
                    if (command == 3) {
                        VkPhysicalDeviceFeatures output = {0};
                        ((PFN_vkGetPhysicalDeviceFeatures)lookup_external(
                            instance, "vkGetPhysicalDeviceFeatures"))(devices[0], &output);
                        assert(!output.robustBufferAccess);
                    } else if (command == 6) {
                        VkPhysicalDeviceProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceProperties"))(devices[0], &output);
                        assert(!output.vendorID);
                    } else if (command == 4) {
                        VkFormatProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceFormatProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceFormatProperties"))(
                            devices[0], VK_FORMAT_R8G8B8A8_UNORM, &output);
                        assert(!output.optimalTilingFeatures);
                    } else if (command == 5) {
                        VkImageFormatProperties output = {0};
                        assert(((PFN_vkGetPhysicalDeviceImageFormatProperties)lookup_external(
                                   instance, "vkGetPhysicalDeviceImageFormatProperties"))(
                                   devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                   VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                   &output) == VK_ERROR_DEVICE_LOST);
                    } else if (command == 7) {
                        VkQueueFamilyProperties output = {0};
                        count = 1;
                        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceQueueFamilyProperties"))(devices[0],
                                                                                   &count, &output);
                        assert(!output.queueCount);
                    } else if (command == 33) {
                        VkSparseImageFormatProperties output = {0};
                        count = 1;
                        ((PFN_vkGetPhysicalDeviceSparseImageFormatProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceSparseImageFormatProperties"))(
                            devices[0], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_TILING_OPTIMAL, &count, &output);
                        assert(!output.aspectMask);
                    } else {
                        VkPhysicalDeviceMemoryProperties output = {0};
                        ((PFN_vkGetPhysicalDeviceMemoryProperties)lookup_external(
                            instance, "vkGetPhysicalDeviceMemoryProperties"))(devices[0], &output);
                        assert(!output.memoryTypeCount);
                    }
                }
            }
            uint32_t number = 99;
            assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
                       instance, "vkEnumeratePhysicalDevices"))(instance, &number, NULL) ==
                   VK_ERROR_DEVICE_LOST);
            VkInstance second = NULL;
            assert(create_function()(&info, NULL, &second) == VK_ERROR_DEVICE_LOST && !second);
        }
        assert(venus_icd_unbind() == RingAgain);
        venus_icd_abandon(); /* Fake receiver owns no live native Vulkan work. */
        assert(venus_icd_unbind() == RingOk);
    }
    fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkBaseInStructure link = {.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO};
    info.pNext = &link;
    instance = NULL;
    assert(create_function()(&info, NULL, &instance) == VK_SUCCESS);
    destroy(instance);
    link.pNext = &link;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    link.pNext = NULL;
    link.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    info.pNext = NULL;
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    fixture.device_count = 0;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    uint32_t none = 99;
    assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(
               instance, "vkEnumeratePhysicalDevices"))(instance, &none, NULL) == VK_SUCCESS &&
           none == 0);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    fixture.fail_fill = 1;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    none = 0;
    assert(
        ((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(
            instance, &none, NULL) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.poll_again = 1001;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.fail_command = 0;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_DEVICE_LOST);
    venus_icd_abandon();
    fixture = fresh();
    fixture.enumerate_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    instance = create();
    uint32_t count = 0;
    assert(
        ((PFN_vkEnumeratePhysicalDevices)lookup_external(instance, "vkEnumeratePhysicalDevices"))(
            instance, &count, NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
    fixture = fresh();
    assert(venus_icd_bind(exchange, &fixture) == RingOk);
    VkInstance instances[16];
    for (unsigned index = 0; index < 16; index++)
        instances[index] = create();
    assert(create_function()(&info, NULL, &instance) == VK_ERROR_OUT_OF_HOST_MEMORY && !instance);
    for (unsigned index = 0; index < 16; index++)
        destroy(instances[index]);
    assert(venus_icd_unbind() == RingOk);
}
#ifdef VgpuIcdLoader
/** @brief Dynamic binding function borrows callback/context until unbind. */
typedef venus_ring_status_t (*binding_t)(venus_command_exchange_t, void *);
/** @brief Dynamic unbinding function owns no storage. */
typedef venus_ring_status_t (*unbinding_t)(void);
#ifdef _WIN32
/** @brief Native test library handle; caller owns until library_close. */
typedef HMODULE loader_library_t;
#else
/** @brief Native test library handle; caller owns until library_close. */
typedef void *loader_library_t;
#endif
/** @brief Open a private native test library; NULL on loader error, caller closes once. */
static loader_library_t library_open(const char *path) {
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
/** @brief Resolve borrowed code address; NULL missing symbol, no ownership/retention. */
static void *library_symbol(loader_library_t library, const char *name) {
#ifdef _WIN32
    FARPROC symbol = GetProcAddress(library, name);
    void *address = NULL;
    _Static_assert(sizeof(address) == sizeof(symbol), "Native loader pointer ABI");
    memcpy(&address, &symbol, sizeof(address));
    return address;
#else
    return dlsym(library, name);
#endif
}
/** @brief Close one nonnull test library after all objects and backend bindings retire. */
static void library_close(loader_library_t library) {
#ifdef _WIN32
    assert(FreeLibrary(library));
#else
    assert(dlclose(library) == 0);
#endif
}
/** @brief Snapshot nullable environment string; caller frees returned owned copy once. */
static char *save_environment(const char *name) {
#ifdef _WIN32
    SetLastError(ERROR_SUCCESS);
    DWORD bytes = GetEnvironmentVariableA(name, NULL, 0);
    if (!bytes) {
        DWORD error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND)
            return NULL;
        assert(error == ERROR_SUCCESS);
        bytes = 1;
    }
    char *copy = malloc(bytes);
    assert(copy);
    copy[0] = 0;
    DWORD written = GetEnvironmentVariableA(name, copy, bytes);
    assert(written < bytes);
#else
    const char *value = getenv(name);
    if (!value)
        return NULL;
    size_t bytes = strlen(value) + 1;
    char *copy = malloc(bytes);
    assert(copy);
    memcpy(copy, value, bytes);
#endif
    return copy;
}
/** @brief Set/remove this test process's environment, borrowed inputs, no retained storage. */
static void set_environment(const char *name, const char *value) {
#ifdef _WIN32
    assert(SetEnvironmentVariableA(name, value));
#else
    assert(value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0);
#endif
}
#ifdef _WIN32
/** @brief Lower only this short-lived test process's integrity to obey loader environment policy.
 * @note Owns/closes token and allocated SID; no system registry/token changes, no guard bypass.
 * Native Windows CI can start elevated; upstream ignores manifest overrides at high integrity.
 */
static void medium_integrity(void) {
    HANDLE token = NULL;
    assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token));
    _Alignas(TOKEN_MANDATORY_LABEL) unsigned char bytes[256];
    DWORD written = 0;
    assert(GetTokenInformation(token, TokenIntegrityLevel, bytes, sizeof(bytes), &written));
    const TOKEN_MANDATORY_LABEL *current = (const TOKEN_MANDATORY_LABEL *)bytes;
    DWORD count = *GetSidSubAuthorityCount(current->Label.Sid);
    assert(count);
    DWORD level = *GetSidSubAuthority(current->Label.Sid, count - 1);
    if (level >= SECURITY_MANDATORY_HIGH_RID) {
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID sid = NULL;
        assert(AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0,
                                        0, 0, &sid));
        TOKEN_MANDATORY_LABEL label = {.Label = {.Sid = sid, .Attributes = SE_GROUP_INTEGRITY}};
        assert(SetTokenInformation(token, TokenIntegrityLevel, &label,
                                   (DWORD)sizeof(label) + GetLengthSid(sid)));
        assert(FreeSid(sid) == NULL);
        sid = NULL;
    }
    assert(CloseHandle(token));
    token = NULL;
}
#endif
static void loader_fixture(void) {
#ifdef _WIN32
    medium_integrity();
    const char *icd_path = "build/waddle_vulkan_experimental.dll";
    const char *manifest_path = "build/waddle_vulkan_experimental_windows.json";
#else
    const char *icd_path = "build/libwaddle_vulkan_experimental.so";
    const char *manifest_path = "build/waddle_vulkan_experimental.json";
#endif
    loader_library_t library = library_open(icd_path);
    const char *loader_path = getenv("WADDLE_TEST_VULKAN_LOADER");
#ifdef _WIN32
    assert(loader_path); /* Native CI must select the exact private pinned loader. */
#else
    if (!loader_path)
        loader_path = "libvulkan.so.1";
#endif
    loader_library_t loader = library_open(loader_path);
    assert(library && loader);
    binding_t bind = NULL;
    unbinding_t unbind = NULL;
    PFN_vkGetInstanceProcAddr lookup = NULL;
    void *address = library_symbol(library, "venus_icd_bind");
    memcpy(&bind, &address, sizeof(bind));
    address = library_symbol(library, "venus_icd_unbind");
    memcpy(&unbind, &address, sizeof(unbind));
    address = library_symbol(loader, "vkGetInstanceProcAddr");
    memcpy(&lookup, &address, sizeof(lookup));
    assert(bind && unbind && lookup);
    char *saved = save_environment("VK_DRIVER_FILES");
    set_environment("VK_DRIVER_FILES", manifest_path);
    fixture_t fixture = fresh();
    assert(bind(exchange, &fixture) == RingOk);
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)lookup(NULL, "vkCreateInstance");
    assert(create);
    for (unsigned iteration = 0; iteration < 8; iteration++) {
        VkInstance instance = NULL;
        VkResult result = create(&info, NULL, &instance);
        if (result != VK_SUCCESS)
            fprintf(stderr, "loader create failed: %d\n", result);
        assert(result == VK_SUCCESS && instance);
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)lookup(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        assert(enumerate && enumerate(instance, &count, NULL) == VK_SUCCESS && count == 2);
        VkPhysicalDevice devices[2];
        assert(enumerate(instance, &count, devices) == VK_SUCCESS && count == 2);
        PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)lookup(instance, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties value = {0};
        assert(properties);
        properties(devices[0], &value);
        assert(value.vendorID == 42);
        PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)lookup(instance, "vkCreateDevice");
        PFN_vkGetDeviceProcAddr device_proc =
            (PFN_vkGetDeviceProcAddr)lookup(instance, "vkGetDeviceProcAddr");
        float priority = 0.5f;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue_info};
        VkDevice device = NULL;
        assert(create_device && device_proc);
        fixture.device_info = &device_info;
        result = create_device(devices[0], &device_info, NULL, &device);
        fixture.device_info = NULL;
        if (result != VK_SUCCESS)
            fprintf(stderr, "loader device create failed: %d\n", result);
        assert(result == VK_SUCCESS && device);
        PFN_vkGetDeviceQueue get_queue =
            (PFN_vkGetDeviceQueue)device_proc(device, "vkGetDeviceQueue");
        PFN_vkQueueWaitIdle queue_idle =
            (PFN_vkQueueWaitIdle)device_proc(device, "vkQueueWaitIdle");
        PFN_vkDeviceWaitIdle device_idle =
            (PFN_vkDeviceWaitIdle)device_proc(device, "vkDeviceWaitIdle");
        PFN_vkCreateFence create_fence = (PFN_vkCreateFence)device_proc(device, "vkCreateFence");
        PFN_vkDestroyFence destroy_fence =
            (PFN_vkDestroyFence)device_proc(device, "vkDestroyFence");
        PFN_vkGetFenceStatus fence_status =
            (PFN_vkGetFenceStatus)device_proc(device, "vkGetFenceStatus");
        PFN_vkWaitForFences wait_fences =
            (PFN_vkWaitForFences)device_proc(device, "vkWaitForFences");
        PFN_vkResetFences reset_fences = (PFN_vkResetFences)device_proc(device, "vkResetFences");
        PFN_vkDestroyDevice destroy_device =
            (PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice");
        assert(get_queue && queue_idle && device_idle && create_fence && destroy_fence &&
               fence_status && wait_fences && reset_fences && destroy_device);
        VkQueue queue = NULL, repeated = NULL;
        get_queue(device, 0, 0, &queue);
        get_queue(device, 0, 0, &repeated);
        assert(queue && repeated == queue);
        assert(queue_idle(queue) == VK_SUCCESS && device_idle(device) == VK_SUCCESS);
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                        .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = NULL;
        assert(create_fence(device, &fence_info, NULL, &fence) == VK_SUCCESS && fence);
        assert(fence_status(device, fence) == VK_SUCCESS);
        assert(wait_fences(device, 1, &fence, VK_TRUE, 0) == VK_SUCCESS);
        assert(reset_fences(device, 1, &fence) == VK_SUCCESS);
        assert(fence_status(device, fence) == VK_NOT_READY);
        assert(wait_fences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
        destroy_fence(device, fence, NULL);
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)device_proc(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)device_proc(device, "vkDestroyBuffer");
        PFN_vkGetBufferMemoryRequirements requirements =
            (PFN_vkGetBufferMemoryRequirements)device_proc(device, "vkGetBufferMemoryRequirements");
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)device_proc(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)device_proc(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)device_proc(device, "vkBindBufferMemory");
        assert(create_buffer && destroy_buffer && requirements && allocate && release && bind);
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        fixture.buffer_info = &buffer_info;
        VkBuffer buffer = NULL;
        assert(create_buffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS && buffer);
        VkMemoryRequirements buffer_memory = {0};
        requirements(device, buffer, &buffer_memory);
        assert(buffer_memory.size == 8192 && buffer_memory.alignment == 256);
        VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = buffer_memory.size, .memoryTypeIndex = 0};
        fixture.memory_info = &memory_info;
        VkDeviceMemory memory = NULL;
        assert(allocate(device, &memory_info, NULL, &memory) == VK_SUCCESS && memory);
        assert(bind(device, buffer, memory, 0) == VK_SUCCESS);
        PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)device_proc(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)device_proc(device, "vkDestroyCommandPool");
        PFN_vkAllocateCommandBuffers allocate_buffers = (PFN_vkAllocateCommandBuffers)device_proc(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers free_buffers = (PFN_vkFreeCommandBuffers)device_proc(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)device_proc(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)device_proc(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)device_proc(device, "vkResetCommandBuffer");
        assert(create_pool && destroy_pool && allocate_buffers && free_buffers && begin && end && reset);
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        fixture.pool_info = &pool_info;
        VkCommandPool pool = NULL;
        assert(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
        VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .commandBufferCount = 2};
        fixture.command_allocate = &command_info;
        VkCommandBuffer commands[2] = {NULL, NULL};
        assert(allocate_buffers(device, &command_info, commands) == VK_SUCCESS && commands[0] && commands[1]);
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        fixture.command_begin = &begin_info;
        PFN_vkCmdFillBuffer fill = (PFN_vkCmdFillBuffer)device_proc(device, "vkCmdFillBuffer");
        assert(fill && begin(commands[0], &begin_info) == VK_SUCCESS);
        fill(commands[0], buffer, 0, VK_WHOLE_SIZE, 0x12345678);
        PFN_vkCmdCopyBuffer copy = (PFN_vkCmdCopyBuffer)device_proc(device, "vkCmdCopyBuffer");
        const VkBufferCopy copy_region = {.srcOffset = 0, .dstOffset = 2048, .size = 1024};
        assert(copy);
        copy(commands[0], buffer, buffer, 1, &copy_region);
        PFN_vkCmdUpdateBuffer update = (PFN_vkCmdUpdateBuffer)device_proc(device, "vkCmdUpdateBuffer");
        const unsigned char update_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        fixture.update_data = update_data;
        assert(update);
        update(commands[0], buffer, 0, sizeof(update_data), update_data);
        PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)device_proc(device, "vkCmdPipelineBarrier");
        const VkMemoryBarrier memory_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        assert(barrier);
        barrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &memory_barrier, 0, NULL, 0, NULL);
        assert(end(commands[0]) == VK_SUCCESS);
        assert(reset(commands[0], 0) == VK_SUCCESS);
        free_buffers(device, pool, 1, commands);
        destroy_pool(device, pool, NULL);
        destroy_buffer(device, buffer, NULL); release(device, memory, NULL);


        PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)device_proc(device, "vkCreateSemaphore");
        PFN_vkDestroySemaphore destroy_semaphore = (PFN_vkDestroySemaphore)device_proc(device, "vkDestroySemaphore");
        const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = NULL;
        assert(create_semaphore && destroy_semaphore);
        assert(create_semaphore(device, &semaphore_info, NULL, &semaphore) == VK_SUCCESS && semaphore);
        destroy_semaphore(device, semaphore, NULL);
        destroy_device(device, NULL);
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)lookup(instance, "vkDestroyInstance");
        assert(destroy);
        destroy(instance, NULL);
    }
    assert(unbind() == RingOk);
    set_environment("VK_DRIVER_FILES", saved);
    free(saved);
    saved = NULL;
    library_close(loader);
    library_close(library);
    puts("Native Vulkan loader: manifest discovery and eight instance/device/queue/fence "
         "lifecycles passed");
}
#endif
/* Mock host push modes0..7: default128,128,256,512,0,126,127,129.
 * Each fresh device validates and caches the actual raw reply before native layout creation.
 * Scalar range alignment remains independent of the host limit's byte granularity.
 */
/** @brief Record exact push bytes without a pipeline binding; borrowed source may change after acknowledgment. */
static void push_range_recording_contract(VkDevice device, PFN_vkGetDeviceProcAddr lookup,
    fixture_t *fixture, VkPipelineLayout layout, uint32_t offset, uint32_t size) {
    PFN_vkCreateCommandPool create_pool=(PFN_vkCreateCommandPool)lookup(device,"vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_pool=(PFN_vkDestroyCommandPool)lookup(device,"vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers allocate=(PFN_vkAllocateCommandBuffers)lookup(device,"vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer begin=(PFN_vkBeginCommandBuffer)lookup(device,"vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end=(PFN_vkEndCommandBuffer)lookup(device,"vkEndCommandBuffer");
    PFN_vkCmdPushConstants push=(PFN_vkCmdPushConstants)lookup(device,"vkCmdPushConstants");
    VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    fixture->pool_info=&pool_info;VkCommandPool pool;assert(create_pool(device,&pool_info,NULL,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.commandBufferCount=1};
    fixture->command_allocate=&command_info;VkCommandBuffer command;assert(allocate(device,&command_info,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};fixture->command_begin=&begin_info;
    assert(begin(command,&begin_info)==VK_SUCCESS);
    unsigned char values[256];for(unsigned index=0;index<sizeof(values);++index)values[index]=(unsigned char)index;
    unsigned before=fixture->submissions;push(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,offset,size,values);
    assert(fixture->submissions==before+1&&fixture->command==132);
    memset(values,0xff,sizeof(values));assert(end(command)==VK_SUCCESS);
    destroy_pool(device,pool,NULL);fixture->pool_info=NULL;fixture->command_allocate=NULL;fixture->command_begin=NULL;
}

/** @brief Prove actual128/256 push limit gates, immutable cache, honest public clamp and sticky hostile replies. */
static void push_limits_contract(void) {
    for(unsigned mode=1;mode<=9;++mode){
        fixture_t fixture=fresh();
        assert(venus_icd_bind(exchange,&fixture)==RingOk);
        VkInstance instance=create();uint32_t count=2;VkPhysicalDevice physical[2];
        assert(((PFN_vkEnumeratePhysicalDevices)lookup_external(instance,"vkEnumeratePhysicalDevices"))(instance,&count,physical)==VK_SUCCESS);
        fixture.descriptor_properties=1;fixture.push_limit_mode=mode<=6?mode:(mode==9?7:2);
        const float priority=1;
        const VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
        const VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
        fixture.device_info=&device_info;VkDevice device;
        assert(((PFN_vkCreateDevice)lookup_external(instance,"vkCreateDevice"))(physical[0],&device_info,NULL,&device)==VK_SUCCESS);
        PFN_vkGetDeviceProcAddr lookup=(PFN_vkGetDeviceProcAddr)lookup_external(instance,"vkGetDeviceProcAddr");
        PFN_vkCreatePipelineLayout create_layout=(PFN_vkCreatePipelineLayout)lookup(device,"vkCreatePipelineLayout");
        PFN_vkDestroyPipelineLayout destroy_layout=(PFN_vkDestroyPipelineLayout)lookup(device,"vkDestroyPipelineLayout");
        VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=4};
        VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
        fixture.pipeline_layout_info=&info;VkPipelineLayout layout=(VkPipelineLayout)(uintptr_t)42;
        if(mode==7)fixture.corrupt_command=6;
        if(mode==8)fixture.fail_command=6;
        unsigned before=fixture.submissions;
        VkResult result=create_layout(device,&info,NULL,&layout);
        if(mode>=4&&mode<=8){
            assert(result==VK_ERROR_DEVICE_LOST&&!layout&&fixture.command==6);
            unsigned after=fixture.submissions;
            assert(create_layout(device,&info,NULL,&layout)==VK_ERROR_DEVICE_LOST&&!layout);
            assert(fixture.submissions==after);
            assert(((PFN_vkDeviceWaitIdle)lookup(device,"vkDeviceWaitIdle"))(device)==VK_ERROR_DEVICE_LOST);
            /* Sole mocked receiver retired; uncertain native state is cleared only by abandon. */
            venus_icd_abandon();continue;
        }
        assert(result==VK_SUCCESS&&layout&&fixture.submissions==before+2&&fixture.command==68);
        destroy_layout(device,layout,NULL);
        VkPhysicalDeviceProperties properties;
        ((PFN_vkGetPhysicalDeviceProperties)lookup_external(instance,"vkGetPhysicalDeviceProperties"))(physical[0],&properties);
        assert(properties.limits.maxPushConstantsSize==(mode==1?128:mode==9?129:256));
        /* Valid actual host limit cached once per device: a later mock reply change
         * must neither change acceptance nor cause a repeated query. */
        fixture.push_limit_mode=4;
        range.size=256;before=fixture.submissions;
        result=create_layout(device,&info,NULL,&layout);
        assert(result==((mode==1||mode==9)?VK_ERROR_INITIALIZATION_FAILED:VK_SUCCESS));
        assert(fixture.submissions==before+((mode==1||mode==9)?0:1));
        if(mode!=1&&mode!=9){assert(layout);push_range_recording_contract(device,lookup,&fixture,layout,0,256);destroy_layout(device,layout,NULL);}else assert(!layout);
        range.offset=252;range.size=4;before=fixture.submissions;
        result=create_layout(device,&info,NULL,&layout);
        assert(result==((mode==1||mode==9)?VK_ERROR_INITIALIZATION_FAILED:VK_SUCCESS));
        assert(fixture.submissions==before+((mode==1||mode==9)?0:1));
        if(mode!=1&&mode!=9){push_range_recording_contract(device,lookup,&fixture,layout,252,4);destroy_layout(device,layout,NULL);}else assert(!layout);
        range.offset=256;before=fixture.submissions;
        assert(create_layout(device,&info,NULL,&layout)==VK_ERROR_INITIALIZATION_FAILED&&!layout);
        assert(fixture.submissions==before);
        range.offset=0;range.size=128;fixture.create_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        assert(create_layout(device,&info,NULL,&layout)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!layout);
        fixture.create_result=VK_SUCCESS;
        assert(create_layout(device,&info,NULL,&layout)==VK_SUCCESS);destroy_layout(device,layout,NULL);
        fixture.push_limit_mode=mode==9?7:mode;
        assert(((PFN_vkDeviceWaitIdle)lookup(device,"vkDeviceWaitIdle"))(device)==VK_SUCCESS);
        ((PFN_vkDestroyDevice)lookup(device,"vkDestroyDevice"))(device,NULL);
        destroy(instance);assert(venus_icd_unbind()==RingOk);
    }
}

int main(void) {
    assert(venus_icd_unbind() == RingOk);
    assert(venus_icd_bind(NULL, NULL) == RingInvalid);
    routing();
    fixture_t fixture = fresh();
    fixture.poll_again = 2;
    fixture.reply_again = 2;
    healthy(&fixture);
    concurrent();
    fence_failures();
    idle_failures();
    idle_concurrency_contract();
    device_failures();
    failures();
    version_contract();
    buffer_contract();
    image_contract();
    descriptor_limits_contract();
    push_limits_contract();
    memory_contract();
    mapping_contract();
    pool_contract();
    command_buffer_contract();
    fill_buffer_contract();
    copy_buffer_contract();
    update_buffer_contract();
    pipeline_barrier_contract();
    semaphore_contract();
    queue_submit_contract();
    venus_icd_abandon();
#ifdef VgpuIcdLoader
    loader_fixture();
#endif
    puts("ICD native instance dispatch: 128 cycles, exact loader aliases and "
         "sticky loss passed");
    return 0;
}
