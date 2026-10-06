/** @file icd.c @brief Native experimental ICD dispatch/lifecycle/loss fixture.
 */
#include "vn_cs.h"
#include "waddle/venus_icd.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#include "vn_protocol_driver_fence.h"
#include "vn_protocol_driver_queue.h"
#pragma GCC diagnostic pop
#include <assert.h>
#include <stdio.h>
#include <string.h>
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
/** @brief Test-only immutable renderer encoder oracle, borrows output for the
 * call.
 * @param[in] kind Pinned query3/6/8.
 * @param[out] bytes Nonnull output[capacity], exclusive to test.
 * @param[in] capacity Actual accessible extent, at least4096.
 * @return Encoded byte extent, zero invalid local input.
 * @note Allocation-free, synchronous; defined in independent values fixture.
 */
size_t venus_values_test_encode(uint32_t kind, void *bytes, size_t capacity);
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
    uint64_t gpu_issued[64];
    uint32_t gpu_pending;
    uint32_t gpu_issue_pending;
    uint32_t gpu_corrupt;
    int32_t gpu_failure;
    unsigned char fence_ready[4096];
    uint32_t fence_pending;
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
        memcpy(output, fixture->reply, capacity);
        response->payload_bytes = (uint32_t)capacity;
    }
    return RingOk;
}
static fixture_t fresh(void) {
    return (fixture_t){.fail_command = UINT32_MAX,
                       .corrupt_command = UINT32_MAX,
                       .transport_failure = RingClosed,
                       .device_count = 2};
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
        assert(!device_lookup(device_handle, "vkCreateBuffer"));
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
        assert(properties.vendorID == 42 && features.robustBufferAccess && memory.memoryTypeCount);
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
    for (unsigned scenario = 0; scenario < 14; scenario++) {
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
    VkFence fences[508], extra = NULL;
    for (unsigned index = 0; index < 508; index++)
        assert(create_fence(device, &fence_info, NULL, &fences[index]) == VK_SUCCESS);
    assert(create_fence(device, &fence_info, NULL, &extra) == VK_ERROR_OUT_OF_HOST_MEMORY &&
           !extra);
    assert(wait_fences(device, 64, fences, VK_TRUE, 0) == VK_SUCCESS);
    assert(reset_fences(device, 64, fences) == VK_SUCCESS);
    for (unsigned index = 0; index < 508; index++)
        destroy_fence(device, fences[index], NULL);
    ((PFN_vkDestroyDevice)lookup(device, "vkDestroyDevice"))(device, NULL);
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
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
    for (unsigned scenario = 0; scenario < 6; scenario++) {
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
        assert(device_create(physical[0], &info, NULL, &devices[index]) == VK_SUCCESS);
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
    destroy(instance);
    assert(venus_icd_unbind() == RingOk);
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
    device_failures();
    failures();
    venus_icd_abandon();
#ifdef VgpuIcdLoader
    loader_fixture();
#endif
    puts("ICD native instance dispatch: 128 cycles, exact loader aliases and "
         "sticky loss passed");
    return 0;
}
