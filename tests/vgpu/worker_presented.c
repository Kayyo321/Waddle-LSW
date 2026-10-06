/** @file worker_presented.c @brief Real presented worker exec, negotiation and
 * channel isolation.
 */
#include "waddle/venus_command.h"
#include "waddle/venus_frame.h"
#include "waddle/venus_guest.h"
#include "waddle/venus_icd.h"
#include "waddle/venus_instance_wire.h"
#include "waddle/venus_objects.h"
#include "waddle/venus_worker.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static venus_ring_status_t (*icd_bind)(venus_command_exchange_t, void *) = venus_icd_bind;
static venus_ring_status_t (*icd_unbind)(void) = venus_icd_unbind;
static void (*icd_abandon)(void) = venus_icd_abandon;
static PFN_vkGetInstanceProcAddr icd_lookup = venus_icd_get_instance_proc_addr;
#ifdef VgpuIcdLoader
#include <dlfcn.h>
static void *icd_library, *loader_library;
static char *saved_driver_files;
static int saved_driver_present, driver_changed;
/** @brief Resolve borrowed function storage; no allocation or pointer conversion.
 * @param[in] library Live owned library and nonnull accessible name.
 * @param[out] output Accessible function storage[bytes], changed only on success.
 * @return Zero success, one missing symbol/unsupported native representation.
 * @note Test sole thread; library must outlive the returned callback.
 */
static int loader_symbol(void *library, const char *name, void *output, size_t bytes) {
    void *symbol = dlsym(library, name);
    if (!symbol || bytes != sizeof(symbol)) return 1;
    memcpy(output, &symbol, bytes);
    return 0;
}
static void loader_cleanup(void) {
    if (loader_library) dlclose(loader_library);
    loader_library = NULL;
    if (icd_library) dlclose(icd_library);
    icd_library = NULL;
    if (driver_changed) {
        if (saved_driver_present) setenv("VK_DRIVER_FILES", saved_driver_files, 1);
        else unsetenv("VK_DRIVER_FILES");
    }
    driver_changed = 0;
    icd_bind = venus_icd_bind; icd_unbind = venus_icd_unbind;
    icd_abandon = venus_icd_abandon; icd_lookup = venus_icd_get_instance_proc_addr;
    free(saved_driver_files);
    saved_driver_files = NULL;
}
static int loader_initialize(void) {
    const char *saved = getenv("VK_DRIVER_FILES");
    saved_driver_present = saved != NULL;
    if (saved) {
        saved_driver_files = strdup(saved);
        if (!saved_driver_files) return 1;
    }
    char manifest[PATH_MAX], library[PATH_MAX];
    const char *loader = getenv("WADDLE_TEST_VULKAN_LOADER");
    if (!loader || !realpath("build/waddle_vulkan_experimental.json", manifest) ||
        !realpath("build/libwaddle_vulkan_experimental.so", library) ||
        setenv("VK_DRIVER_FILES", manifest, 1)) return 1;
    driver_changed = 1;
    icd_library = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    loader_library = dlopen(loader, RTLD_NOW | RTLD_LOCAL);
    if (!icd_library || !loader_library) return 1;
    return loader_symbol(icd_library, "venus_icd_bind", &icd_bind, sizeof(icd_bind)) ||
        loader_symbol(icd_library, "venus_icd_unbind", &icd_unbind, sizeof(icd_unbind)) ||
        loader_symbol(icd_library, "venus_icd_abandon", &icd_abandon, sizeof(icd_abandon)) ||
        loader_symbol(loader_library, "vkGetInstanceProcAddr", &icd_lookup, sizeof(icd_lookup));
}
#endif
static unsigned descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory)
        return 0;
    unsigned count = 0;
    while (readdir(directory))
        count++;
    if (closedir(directory))
        return 0;
    return count;
}
static venus_ring_status_t command_exchange(void *context, const venus_request_t *request,
                                            const void *input, size_t length,
                                            venus_request_t *response, void *output,
                                            size_t capacity) {
    return venus_guest_exchange(context, request, input, length, response, output, capacity);
}
static int query_version(venus_guest_t *guest) {
    static const unsigned char VersionCommand[16] = {137, 0, 0, 0, 1, 0, 0, 0,
                                                     1,   0, 0, 0, 0, 0, 0, 0};
    static const unsigned char ReplyHeader[16] = {137, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    venus_command_t command = {0};
    unsigned char tx[128], rx[32];
    if (venus_command_init(&command, command_exchange, guest, tx, sizeof(tx), rx, sizeof(rx)) !=
        RingOk)
        return 1;
    for (unsigned iteration = 0; iteration < 8; iteration++) {
        if (venus_command_start(&command, VersionCommand, sizeof(VersionCommand)) != RingOk)
            return 1; /* Caller abandons the entire old session on any failure. */
        for (unsigned attempt = 0;; attempt++) {
            venus_ring_status_t status = venus_command_poll(&command);
            if (status == RingOk)
                break;
            if (status != RingAgain || attempt >= 1000)
                return 1;
            usleep(1000);
        }
        const void *view = NULL;
        size_t bytes = 0;
        if (venus_command_take(&command, &view, &bytes) != RingOk || bytes != sizeof(rx) ||
            memcmp(view, ReplyHeader, sizeof(ReplyHeader)) || !rx[18])
            return 1;
    }
    venus_command_free(&command);
    return 0;
}
static venus_ring_status_t wait_command(venus_command_t *command, const void **view,
                                        size_t *length) {
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        venus_ring_status_t status = venus_command_poll(command);
        if (status == RingOk)
            return venus_command_take(command, view, length);
        if (status != RingAgain)
            return status;
        usleep(1000);
    }
    return RingTimeout;
}
static int instance_cycle(venus_guest_t *guest) {
    static uint32_t next_namespace = 1; /* Sole test thread; never reuse a session namespace. */
    int result = 1;
    venus_command_t command = {0};
    venus_objects_t objects = {0};
    venus_object_t slots[4], *instance = NULL;
    unsigned char tx[256], rx[32], encoded[128];
    size_t written = 0, length = 0;
    const void *view = NULL;
    venus_vk_instance_info_t info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    venus_vk_result_t create_result = VK_ERROR_UNKNOWN;
    if (next_namespace == UINT32_MAX ||
        venus_objects_init(&objects, slots, 4, next_namespace++, guest) != RingOk ||
        venus_command_init(&command, command_exchange, guest, tx, sizeof(tx), rx, sizeof(rx)) !=
            RingOk ||
        venus_objects_reserve(&objects, 1, 0, 1, &instance) != RingOk)
        goto cleanup;
    if (venus_instance_wire_create(&info, instance->id, encoded, sizeof(encoded), &written) !=
            RingOk ||
        venus_command_start(&command, encoded, written) != RingOk ||
        wait_command(&command, &view, &length) != RingOk ||
        venus_instance_wire_create_reply(&create_result, view, length, instance->id) != RingOk ||
        create_result != VK_SUCCESS)
        goto cleanup;
    if (venus_instance_wire_destroy(instance->id, encoded, sizeof(encoded), &written) != RingOk ||
        venus_command_start(&command, encoded, written) != RingOk ||
        wait_command(&command, &view, &length) != RingOk ||
        venus_objects_release(&objects, instance->handle, 1, 1) != RingOk || objects.live_count)
        goto cleanup;
    result = 0;
cleanup:
    if (result)
        venus_guest_free(guest); /* Abandon transport before forgetting any
                                    outstanding object. */
    venus_command_free(&command);
    venus_objects_free(&objects);
    return result;
}
static int icd_cycles(venus_guest_t *guest) {
#ifdef VgpuIcdLoader
    /* Host receiver is already initialized with its original driver environment. */
    if (loader_initialize()) { loader_cleanup(); return 1; }
#endif
    if (icd_bind(command_exchange, guest) != RingOk)
        return 1;
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)icd_lookup(NULL, "vkCreateInstance");
    VkInstance cleanup_instance = NULL;
    VkDevice cleanup_device = NULL;
    PFN_vkDestroyInstance cleanup_destroy_instance = NULL;
    PFN_vkDestroyDevice cleanup_destroy_device = NULL;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    for (unsigned iteration = 0; iteration < 8; iteration++) {
        VkInstance instance = NULL;
        if (!create || create(&info, NULL, &instance) != VK_SUCCESS || !instance)
            goto fail;
        cleanup_instance = instance;
        cleanup_destroy_instance = (PFN_vkDestroyInstance)icd_lookup(instance, "vkDestroyInstance");
        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)icd_lookup(
                instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        if (!enumerate || enumerate(instance, &count, NULL) != VK_SUCCESS || !count || count > 16)
            goto fail;
        VkPhysicalDevice devices[16] = {0};
        uint32_t capacity = count;
        if (enumerate(instance, &capacity, devices) != VK_SUCCESS || capacity != count)
            goto fail;
        PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)icd_lookup(
                instance, "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceFeatures features =
            (PFN_vkGetPhysicalDeviceFeatures)icd_lookup(
                instance, "vkGetPhysicalDeviceFeatures");
        PFN_vkGetPhysicalDeviceMemoryProperties memory =
            (PFN_vkGetPhysicalDeviceMemoryProperties)icd_lookup(
                instance, "vkGetPhysicalDeviceMemoryProperties");
        if (!properties || !features || !memory)
            goto fail;
        for (uint32_t index = 0; index < count; index++) {
            VkPhysicalDeviceProperties property_value = {0};
            VkPhysicalDeviceFeatures feature_value = {0};
            VkPhysicalDeviceMemoryProperties memory_value = {0};
            properties(devices[index], &property_value);
            features(devices[index], &feature_value);
            memory(devices[index], &memory_value);
            if (property_value.apiVersion < VK_API_VERSION_1_0 || !property_value.deviceName[0] ||
                !memory_value.memoryTypeCount || !memory_value.memoryHeapCount)
                goto fail;
        }
        PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)icd_lookup(
                instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        PFN_vkCreateDevice create_device =
            (PFN_vkCreateDevice)icd_lookup(instance, "vkCreateDevice");
        PFN_vkGetDeviceProcAddr device_proc =
            (PFN_vkGetDeviceProcAddr)icd_lookup(instance,
                                                                      "vkGetDeviceProcAddr");
        if (!queue_properties || !create_device || !device_proc)
            goto fail;
        uint32_t family_count = 64;
        VkQueueFamilyProperties families[64] = {0};
        queue_properties(devices[0], &family_count, families);
        uint32_t family = 0;
        while (family < family_count && !families[family].queueCount)
            family++;
        if (!family_count || family_count > 64 || family == family_count)
            goto fail;
        float priority = 0.5f;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = family,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue_info};
        VkDevice device = NULL;
        if (create_device(devices[0], &device_info, NULL, &device) != VK_SUCCESS || !device)
            goto fail;
        cleanup_device = device;
        cleanup_destroy_device = (PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice");
        PFN_vkGetDeviceQueue get_queue =
            (PFN_vkGetDeviceQueue)device_proc(device, "vkGetDeviceQueue");
        PFN_vkDestroyDevice destroy_device =
            (PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice");
        PFN_vkDeviceWaitIdle device_idle =
            (PFN_vkDeviceWaitIdle)device_proc(device, "vkDeviceWaitIdle");
        PFN_vkQueueWaitIdle queue_idle =
            (PFN_vkQueueWaitIdle)device_proc(device, "vkQueueWaitIdle");
        if (!get_queue || !destroy_device || !device_idle || !queue_idle)
            goto fail;
        VkQueue queue = NULL, repeated = NULL;
        get_queue(device, family, 0, &queue);
        get_queue(device, family, 0, &repeated);
        if (!queue || queue != repeated || queue_idle(queue) != VK_SUCCESS ||
            device_idle(device) != VK_SUCCESS)
            goto fail;
        PFN_vkCreateFence create_fence = (PFN_vkCreateFence)device_proc(device, "vkCreateFence");
        PFN_vkDestroyFence destroy_fence =
            (PFN_vkDestroyFence)device_proc(device, "vkDestroyFence");
        PFN_vkResetFences reset_fences = (PFN_vkResetFences)device_proc(device, "vkResetFences");
        PFN_vkGetFenceStatus fence_status =
            (PFN_vkGetFenceStatus)device_proc(device, "vkGetFenceStatus");
        PFN_vkWaitForFences wait_fences =
            (PFN_vkWaitForFences)device_proc(device, "vkWaitForFences");
        if (!create_fence || !destroy_fence || !reset_fences || !fence_status || !wait_fences)
            goto fail;
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                        .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = NULL;
        if (create_fence(device, &fence_info, NULL, &fence) != VK_SUCCESS || !fence ||
            fence_status(device, fence) != VK_SUCCESS ||
            wait_fences(device, 1, &fence, VK_TRUE, 0) != VK_SUCCESS ||
            reset_fences(device, 1, &fence) != VK_SUCCESS ||
            fence_status(device, fence) != VK_NOT_READY ||
            wait_fences(device, 1, &fence, VK_TRUE, 0) != VK_TIMEOUT)
            goto fail;
        destroy_fence(device, fence, NULL);
        PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)device_proc(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)device_proc(device, "vkDestroyBuffer");
        PFN_vkGetBufferMemoryRequirements requirements =
            (PFN_vkGetBufferMemoryRequirements)device_proc(device, "vkGetBufferMemoryRequirements");
        if (!create_buffer || !destroy_buffer || !requirements) goto fail;
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 65536, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer buffer = NULL;
        if (create_buffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS || !buffer) goto fail;
        VkMemoryRequirements buffer_memory = {0};
        requirements(device, buffer, &buffer_memory);
        if (buffer_memory.size < 4096 || !buffer_memory.alignment || !buffer_memory.memoryTypeBits) goto fail;
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)device_proc(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)device_proc(device, "vkFreeMemory");
        PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)device_proc(device, "vkBindBufferMemory");
        if (!allocate || !release || !bind) goto fail;
        uint32_t memory_type = 0;
        while (!(buffer_memory.memoryTypeBits & (1u << memory_type))) memory_type++;
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = buffer_memory.size, .memoryTypeIndex = memory_type};
        VkDeviceMemory buffer_allocation = NULL;
        if (allocate(device, &allocation, NULL, &buffer_allocation) != VK_SUCCESS ||
            !buffer_allocation || bind(device, buffer, buffer_allocation, 0) != VK_SUCCESS ||
            device_idle(device) != VK_SUCCESS) goto fail;
#ifdef VgpuIcdLoader
        if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto fail;
#endif
        PFN_vkCreateCommandPool create_pool =
            (PFN_vkCreateCommandPool)device_proc(device, "vkCreateCommandPool");
        PFN_vkDestroyCommandPool destroy_pool =
            (PFN_vkDestroyCommandPool)device_proc(device, "vkDestroyCommandPool");
        PFN_vkResetCommandPool reset_pool =
            (PFN_vkResetCommandPool)device_proc(device, "vkResetCommandPool");
        if (!create_pool || !destroy_pool || !reset_pool) goto fail;
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        VkCommandPool pool = NULL;
        if (create_pool(device, &pool_info, NULL, &pool) != VK_SUCCESS || !pool ||
            reset_pool(device, pool, 0) != VK_SUCCESS ||
            reset_pool(device, pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != VK_SUCCESS)
            goto fail;
        PFN_vkAllocateCommandBuffers allocate_buffers =
            (PFN_vkAllocateCommandBuffers)device_proc(device, "vkAllocateCommandBuffers");
        PFN_vkFreeCommandBuffers free_buffers =
            (PFN_vkFreeCommandBuffers)device_proc(device, "vkFreeCommandBuffers");
        PFN_vkBeginCommandBuffer begin_buffer =
            (PFN_vkBeginCommandBuffer)device_proc(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end_buffer =
            (PFN_vkEndCommandBuffer)device_proc(device, "vkEndCommandBuffer");
        PFN_vkResetCommandBuffer reset_buffer =
            (PFN_vkResetCommandBuffer)device_proc(device, "vkResetCommandBuffer");
        PFN_vkCmdFillBuffer fill_buffer =
            (PFN_vkCmdFillBuffer)device_proc(device, "vkCmdFillBuffer");
        PFN_vkCmdCopyBuffer copy_buffer =
            (PFN_vkCmdCopyBuffer)device_proc(device, "vkCmdCopyBuffer");
        PFN_vkCmdUpdateBuffer update_buffer =
            (PFN_vkCmdUpdateBuffer)device_proc(device, "vkCmdUpdateBuffer");
        PFN_vkCmdPipelineBarrier pipeline_barrier =
            (PFN_vkCmdPipelineBarrier)device_proc(device, "vkCmdPipelineBarrier");
        if (!allocate_buffers || !free_buffers || !begin_buffer || !end_buffer || !reset_buffer ||
            !fill_buffer || !copy_buffer || !update_buffer || !pipeline_barrier)
            goto fail;
        VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VkCommandBuffer commands[2] = {NULL, NULL};
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (allocate_buffers(device, &command_info, commands) != VK_SUCCESS || !commands[0] ||
            !commands[1] || begin_buffer(commands[0], &begin_info) != VK_SUCCESS) goto fail;
        fill_buffer(commands[0], buffer, 0, VK_WHOLE_SIZE, 0x12345678);
        VkMemoryBarrier memory_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        pipeline_barrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &memory_barrier, 0, NULL, 0, NULL);
        const VkBufferCopy copy_region = {.srcOffset = 0, .dstOffset = 2048, .size = 1024};
        copy_buffer(commands[0], buffer, buffer, 1, &copy_region);
        pipeline_barrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &memory_barrier, 0, NULL, 0, NULL);
        unsigned char update_data[65536];
        for (unsigned index = 0; index < sizeof(update_data); index++)
            update_data[index] = (unsigned char)(index * 13);
        update_buffer(commands[0], buffer, 0, sizeof(update_data), update_data);
        memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        pipeline_barrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &memory_barrier, 0, NULL, 0, NULL);
        if (end_buffer(commands[0]) != VK_SUCCESS || reset_buffer(commands[0], 0) != VK_SUCCESS ||
            begin_buffer(commands[1], &begin_info) != VK_SUCCESS ||
            end_buffer(commands[1]) != VK_SUCCESS || reset_pool(device, pool, 0) != VK_SUCCESS)
            goto fail;
        free_buffers(device, pool, 1, commands);
        /* Pool destruction implicitly retires the second loader-dispatchable buffer. */
        destroy_pool(device, pool, NULL);
        destroy_buffer(device, buffer, NULL);
        release(device, buffer_allocation, NULL);

        destroy_device(device, NULL);
        cleanup_device = NULL;
#ifndef VgpuIcdLoader
        if (device_proc(device, "vkDestroyDevice"))
            goto fail;
#endif
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)icd_lookup(instance, "vkDestroyInstance");
        if (!destroy)
            goto fail;
        destroy(instance, NULL);
        cleanup_instance = NULL;
#ifndef VgpuIcdLoader
        if (icd_lookup(instance, "vkDestroyInstance"))
            goto fail;
#endif
    }
    if (icd_unbind() == RingOk) {
#ifdef VgpuIcdLoader
        loader_cleanup();
#endif
        return 0;
    }
fail:
    /* Release loader-owned CPU dispatch tables even when host ownership is uncertain. */
    if (cleanup_device && cleanup_destroy_device) cleanup_destroy_device(cleanup_device, NULL);
    if (cleanup_instance && cleanup_destroy_instance) cleanup_destroy_instance(cleanup_instance, NULL);
    venus_guest_free(guest); /* Stop receiver access before forgetting reserved objects. */
    return 1;
}
static int run_fixture(int corrupt) {
    int result = 1, mapping_fd = -1, streams[2] = {-1, -1}, frames[2] = {-1, -1};
    void *mapping = MAP_FAILED;
    venus_worker_t worker = {0};
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    venus_guest_t guest = {0};
    unsigned char scratch[131072], bytes[VenusFrameBytes], completion[VenusReleaseBytes];
    char executable[PATH_MAX];
    const char *configured = getenv("WADDLE_PRODUCTION_WORKER");
    if (!realpath(configured ? configured : "build/waddle_vgpu_worker", executable))
        goto cleanup;
    mapping_fd = memfd_create("presented-worker", MFD_CLOEXEC);
    if (mapping_fd < 0 || ftruncate(mapping_fd, 4096) != 0)
        goto cleanup;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, 0);
    if (mapping == MAP_FAILED || venus_region_init(mapping, 4096, 64) != RingOk ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, streams) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, frames) != 0 ||
        venus_frame_prepare(frames[0]) != RingOk || venus_frame_prepare(frames[1]) != RingOk)
        goto cleanup;
    if (venus_worker_create_presented(&worker, executable, mapping_fd, streams[1], frames[1],
                                      UINT64_MAX) != RingOk)
        goto cleanup;
    close(streams[1]);
    streams[1] = -1;
    close(frames[1]);
    frames[1] = -1;
    if (venus_session_init(&session, SessionGuest, mapping, 4096, 0) != RingOk ||
        venus_channel_init(&channel, &session, streams[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) != RingOk ||
        venus_guest_init(&guest, &rpc, 5000) != RingOk)
        goto cleanup;
    if (query_version(&guest) || instance_cycle(&guest) || icd_cycles(&guest))
        goto cleanup;
    venus_frame_t frame = {.context = UINT64_MAX,
                           .frame = 1,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.width = 32, .height = 16}}};
    venus_request_t request = {.kind = RequestPresent,
                               .payload_bytes = sizeof(bytes),
                               .argument_zero = 1,
                               .argument_one = 1},
                    response;
    if (venus_frame_encode(&frame, bytes, sizeof(bytes)) != RingOk ||
        venus_guest_exchange(&guest, &request, bytes, sizeof(bytes), &response, NULL, 0) !=
            RingInvalid)
        goto cleanup; /* No registered allocation/fence: never publish a frame. */
    request = (venus_request_t){.kind = RequestPresentPoll, .argument_zero = 1};
    if (venus_guest_exchange(&guest, &request, NULL, 0, &response, completion,
                             sizeof(completion)) != RingInvalid)
        goto cleanup;
    int received[4];
    if (venus_frame_receive(frames[0], worker.process_id, UINT64_MAX, &frame, received) !=
        RingAgain)
        goto cleanup;
    if (corrupt) {
        const venus_release_t Unknown = {.context = UINT64_MAX, .frame = 1, .status = RingOk};
        if (venus_release_send(frames[0], &Unknown) != RingOk)
            goto cleanup;
        request = (venus_request_t){.kind = RequestPoll};
        venus_ring_status_t status =
            venus_guest_exchange(&guest, &request, NULL, 0, &response, NULL, 0);
        if (status != RingClosed && status != RingCorrupt)
            goto cleanup;
    }
    venus_guest_free(&guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    close(streams[0]);
    streams[0] = -1;
    for (unsigned attempt = 0;; attempt++) {
        venus_ring_status_t status = venus_worker_poll(&worker);
        if (status == RingClosed) {
            if (!WIFEXITED(worker.exit_status) || WEXITSTATUS(worker.exit_status) != corrupt)
                goto cleanup;
            break;
        }
        if (status != RingAgain || attempt >= 5000)
            goto cleanup;
        usleep(1000);
    }
    result = 0;
cleanup:
    venus_guest_free(&guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    if (venus_worker_destroy(&worker, 1000) != RingOk)
        result = 1;
    if (result) icd_abandon(); /* Receiver retired before forgetting uncertain objects. */
#ifdef VgpuIcdLoader
    loader_cleanup();
#endif
    for (unsigned index = 0; index < 2; index++) {
        if (streams[index] >= 0)
            close(streams[index]);
        if (frames[index] >= 0)
            close(frames[index]);
    }
    venus_region_detach(&session.region);
    if (mapping != MAP_FAILED)
        munmap(mapping, 4096);
    if (mapping_fd >= 0)
        close(mapping_fd);
    return result;
}
int main(void) {
    alarm(180);
    int result = 1;
    unsigned baseline = descriptors();
    if (!baseline) goto cleanup;
    for (unsigned corrupt = 0; corrupt < 2; corrupt++) {
        int fixture_result = run_fixture((int)corrupt);
        unsigned after = descriptors();
        if (fixture_result || after != baseline) {
            result = after == baseline ? 1 : 2;
            fprintf(stderr, "Presented worker binding failed: mode=%u\n", corrupt);
            goto cleanup;
        }
    }
    puts("Presented production worker negotiation, isolation and unknown-release shutdown passed");
    result = 0;
cleanup:
#ifdef VgpuIcdLoader
    loader_cleanup();
#endif
    return result;
}
