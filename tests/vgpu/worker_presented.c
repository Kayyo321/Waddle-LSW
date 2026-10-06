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
#include "shaders/compute_shader.h"
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
/** @brief Test workload policy; immutable after main parses trusted test configuration. */
typedef enum fixture_workload_t {
    FullWorkload, /**< Default mapped transfer plus image lifecycle acceptance. */
    MappingWorkload, /**< Exact-byte mapped CPU/GPU transfer workload. */
    ImageWorkload, /**< Native image/view lifecycle and subsequent image commands. */
    ComputeWorkload /**< Actual shader dispatch and exact mapped output comparison. */
} fixture_workload_t;
static fixture_workload_t selected_workload = FullWorkload;
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
/** @brief Look up a borrowed compute entry point and name any unavailable API.
 * @param[in] device Live borrowed device.
 * @param[in] device_proc Nonnull borrowed dispatcher, valid for device lifetime.
 * @param[in] name Nonnull borrowed NUL-terminated static Vulkan API name.
 * @return Borrowed function pointer or NULL; retains nothing, no allocation.
 * @details Single-threaded diagnostic fixture; dispatcher determines thread safety.
 */
static PFN_vkVoidFunction compute_proc(VkDevice device, PFN_vkGetDeviceProcAddr device_proc,
    const char *name) {
    PFN_vkVoidFunction function = device_proc(device, name);
    if (!function) fprintf(stderr, "ICD compute acceptance missing entry point: %s\n", name);
    return function;
}
/** @brief Execute actual storage-buffer shader and compare every result word.
 * @param[in] device Borrowed live device, retained by caller until return.
 * @param[in] queue Borrowed compute-capable queue of family.
 * @param[in] family Existing queue family index.
 * @param[in] supported_memory Borrowed queried actual guest memory properties.
 * @param[in] device_proc Nonnull live device dispatch lookup.
 * @return 0 on exact64-word GPU proof,1 on failure; single-threaded fixture.
 * Local resource owner is this function; cleanup releases every acquired resource.
 * supported_memory has1..VK_MAX_MEMORY_TYPES validated records.
 */
static int compute_probe(VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *supported_memory, PFN_vkGetDeviceProcAddr device_proc) {
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)compute_proc(device, device_proc, "vkCreateBuffer");
    if (!create_buffer) return 1;
    PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)compute_proc(device, device_proc, "vkDestroyBuffer");
    if (!destroy_buffer) return 1;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = (PFN_vkGetBufferMemoryRequirements)compute_proc(device, device_proc, "vkGetBufferMemoryRequirements");
    if (!get_buffer_memory_requirements) return 1;
    PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)compute_proc(device, device_proc, "vkAllocateMemory");
    if (!allocate_memory) return 1;
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)compute_proc(device, device_proc, "vkFreeMemory");
    if (!free_memory) return 1;
    PFN_vkBindBufferMemory bind_buffer_memory = (PFN_vkBindBufferMemory)compute_proc(device, device_proc, "vkBindBufferMemory");
    if (!bind_buffer_memory) return 1;
    PFN_vkMapMemory map_memory = (PFN_vkMapMemory)compute_proc(device, device_proc, "vkMapMemory");
    if (!map_memory) return 1;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges =
        (PFN_vkFlushMappedMemoryRanges)compute_proc(device, device_proc, "vkFlushMappedMemoryRanges");
    if (!flush_mapped_memory_ranges) return 1;
    PFN_vkUnmapMemory unmap_memory = (PFN_vkUnmapMemory)compute_proc(device, device_proc, "vkUnmapMemory");
    if (!unmap_memory) return 1;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = (PFN_vkInvalidateMappedMemoryRanges)compute_proc(device, device_proc, "vkInvalidateMappedMemoryRanges");
    if (!invalidate_mapped_memory_ranges) return 1;
    PFN_vkCreateShaderModule create_shader_module = (PFN_vkCreateShaderModule)compute_proc(device, device_proc, "vkCreateShaderModule");
    if (!create_shader_module) return 1;
    PFN_vkDestroyShaderModule destroy_shader_module = (PFN_vkDestroyShaderModule)compute_proc(device, device_proc, "vkDestroyShaderModule");
    if (!destroy_shader_module) return 1;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = (PFN_vkCreateDescriptorSetLayout)compute_proc(device, device_proc, "vkCreateDescriptorSetLayout");
    if (!create_descriptor_set_layout) return 1;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = (PFN_vkDestroyDescriptorSetLayout)compute_proc(device, device_proc, "vkDestroyDescriptorSetLayout");
    if (!destroy_descriptor_set_layout) return 1;
    PFN_vkCreatePipelineLayout create_pipeline_layout = (PFN_vkCreatePipelineLayout)compute_proc(device, device_proc, "vkCreatePipelineLayout");
    if (!create_pipeline_layout) return 1;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = (PFN_vkDestroyPipelineLayout)compute_proc(device, device_proc, "vkDestroyPipelineLayout");
    if (!destroy_pipeline_layout) return 1;
    PFN_vkCreateDescriptorPool create_descriptor_pool = (PFN_vkCreateDescriptorPool)compute_proc(device, device_proc, "vkCreateDescriptorPool");
    if (!create_descriptor_pool) return 1;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = (PFN_vkDestroyDescriptorPool)compute_proc(device, device_proc, "vkDestroyDescriptorPool");
    if (!destroy_descriptor_pool) return 1;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = (PFN_vkAllocateDescriptorSets)compute_proc(device, device_proc, "vkAllocateDescriptorSets");
    if (!allocate_descriptor_sets) return 1;
    PFN_vkUpdateDescriptorSets update_descriptor_sets = (PFN_vkUpdateDescriptorSets)compute_proc(device, device_proc, "vkUpdateDescriptorSets");
    if (!update_descriptor_sets) return 1;
    PFN_vkCreateComputePipelines create_compute_pipelines = (PFN_vkCreateComputePipelines)compute_proc(device, device_proc, "vkCreateComputePipelines");
    if (!create_compute_pipelines) return 1;
    PFN_vkDestroyPipeline destroy_pipeline = (PFN_vkDestroyPipeline)compute_proc(device, device_proc, "vkDestroyPipeline");
    if (!destroy_pipeline) return 1;
    PFN_vkCreateCommandPool create_command_pool = (PFN_vkCreateCommandPool)compute_proc(device, device_proc, "vkCreateCommandPool");
    if (!create_command_pool) return 1;
    PFN_vkDestroyCommandPool destroy_command_pool = (PFN_vkDestroyCommandPool)compute_proc(device, device_proc, "vkDestroyCommandPool");
    if (!destroy_command_pool) return 1;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = (PFN_vkAllocateCommandBuffers)compute_proc(device, device_proc, "vkAllocateCommandBuffers");
    if (!allocate_command_buffers) return 1;
    PFN_vkBeginCommandBuffer begin_command_buffer = (PFN_vkBeginCommandBuffer)compute_proc(device, device_proc, "vkBeginCommandBuffer");
    if (!begin_command_buffer) return 1;
    PFN_vkEndCommandBuffer end_command_buffer = (PFN_vkEndCommandBuffer)compute_proc(device, device_proc, "vkEndCommandBuffer");
    if (!end_command_buffer) return 1;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = (PFN_vkCmdBindPipeline)compute_proc(device, device_proc, "vkCmdBindPipeline");
    if (!cmd_bind_pipeline) return 1;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = (PFN_vkCmdBindDescriptorSets)compute_proc(device, device_proc, "vkCmdBindDescriptorSets");
    if (!cmd_bind_descriptor_sets) return 1;
    PFN_vkCmdDispatch cmd_dispatch = (PFN_vkCmdDispatch)compute_proc(device, device_proc, "vkCmdDispatch");
    if (!cmd_dispatch) return 1;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = (PFN_vkCmdPipelineBarrier)compute_proc(device, device_proc, "vkCmdPipelineBarrier");
    if (!cmd_pipeline_barrier) return 1;
    PFN_vkCreateFence create_fence = (PFN_vkCreateFence)compute_proc(device, device_proc, "vkCreateFence");
    if (!create_fence) return 1;
    PFN_vkDestroyFence destroy_fence = (PFN_vkDestroyFence)compute_proc(device, device_proc, "vkDestroyFence");
    if (!destroy_fence) return 1;
    PFN_vkQueueSubmit queue_submit = (PFN_vkQueueSubmit)compute_proc(device, device_proc, "vkQueueSubmit");
    if (!queue_submit) return 1;
    PFN_vkWaitForFences wait_for_fences = (PFN_vkWaitForFences)compute_proc(device, device_proc, "vkWaitForFences");
    if (!wait_for_fences) return 1;
    PFN_vkQueueWaitIdle queue_wait_idle = (PFN_vkQueueWaitIdle)compute_proc(device, device_proc, "vkQueueWaitIdle");
    if (!queue_wait_idle) return 1;
    int result = 1, submitted = 0;
    const char *stage = "buffer acquisition";
    VkBuffer buffer = NULL;
    VkDeviceMemory allocation = NULL;
    void *mapped = NULL;
    VkShaderModule shader = NULL;
    VkDescriptorSetLayout set_layout = NULL;
    VkPipelineLayout pipeline_layout = NULL;
    VkDescriptorPool descriptor_pool = NULL;
    VkPipeline pipeline = NULL;
    VkCommandPool command_pool = NULL;
    VkFence fence = NULL;
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (create_buffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS || !buffer) goto cleanup;
    VkMemoryRequirements memory_requirements = {0};
    get_buffer_memory_requirements(device, buffer, &memory_requirements);
    uint32_t memory_type = VK_MAX_MEMORY_TYPES;
    for (uint32_t index = 0; index < supported_memory->memoryTypeCount; index++) {
        if ((memory_requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
            (supported_memory->memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memory_type = index; break;
        }
    }
    if (memory_type == VK_MAX_MEMORY_TYPES) goto cleanup;
    const VkMemoryAllocateInfo allocation_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size, .memoryTypeIndex = memory_type};
    if (allocate_memory(device, &allocation_info, NULL, &allocation) != VK_SUCCESS || !allocation ||
        bind_buffer_memory(device, buffer, allocation, 0) != VK_SUCCESS ||
        map_memory(device, allocation, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) goto cleanup;
    memset(mapped, 0xa5, 256);
    const VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = allocation, .size = VK_WHOLE_SIZE};
    stage = "compute mapped poison and flush";
    if (flush_mapped_memory_ranges(device, 1, &range) != VK_SUCCESS) goto cleanup;
#ifdef VgpuIcdLoader
    if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto cleanup;
#endif
    stage = "shader and descriptor acquisition";
    const VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(ComputeShader), .pCode = ComputeShader};
    if (create_shader_module(device, &shader_info, NULL, &shader) != VK_SUCCESS || !shader) goto cleanup;
    const VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo set_layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    if (create_descriptor_set_layout(device, &set_layout_info, NULL, &set_layout) != VK_SUCCESS || !set_layout) goto cleanup;
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    if (create_pipeline_layout(device, &pipeline_layout_info, NULL, &pipeline_layout) != VK_SUCCESS || !pipeline_layout) goto cleanup;
    const VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    const VkDescriptorPoolCreateInfo descriptor_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    if (create_descriptor_pool(device, &descriptor_pool_info, NULL, &descriptor_pool) != VK_SUCCESS || !descriptor_pool) goto cleanup;
    const VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet descriptor_set = NULL;
    if (allocate_descriptor_sets(device, &set_info, &descriptor_set) != VK_SUCCESS || !descriptor_set) goto cleanup;
    const VkDescriptorBufferInfo descriptor_buffer = {.buffer = buffer, .range = 256};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &descriptor_buffer};
    update_descriptor_sets(device, 1, &write, 0, NULL);
    stage = "compute pipeline creation";
    const VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
        .layout = pipeline_layout, .basePipelineIndex = -1};
    if (create_compute_pipelines(device, NULL, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS || !pipeline) goto cleanup;
    const VkCommandPoolCreateInfo command_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    if (create_command_pool(device, &command_pool_info, NULL, &command_pool) != VK_SUCCESS || !command_pool) goto cleanup;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command = NULL;
    if (allocate_command_buffers(device, &command_info, &command) != VK_SUCCESS || !command) goto cleanup;
    const VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (begin_command_buffer(command, &begin_info) != VK_SUCCESS) goto cleanup;
    stage = "compute command recording";
    cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    cmd_bind_descriptor_sets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    cmd_dispatch(command, 64, 1, 1);
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (end_command_buffer(command) != VK_SUCCESS ||
        create_fence(device, &fence_info, NULL, &fence) != VK_SUCCESS || !fence) goto cleanup;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    stage = "compute submission";
    if (queue_submit(queue, 1, &submit, fence) != VK_SUCCESS) goto cleanup;
    submitted = 1;
    if (wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
        queue_wait_idle(queue) != VK_SUCCESS) goto cleanup;
    submitted = 0;
    stage = "compute mapped output comparison";
    if (invalidate_mapped_memory_ranges(device, 1, &range) != VK_SUCCESS) goto cleanup;
    const uint32_t *words = mapped;
    for (uint32_t index = 0; index < 64; index++) {
        if (words[index] != index * 13 + 7) {
            fprintf(stderr, "ICD compute output word%u: got0x%08x, expected0x%08x\n",
                index, words[index], index * 13 + 7);
            goto cleanup;
        }
    }
    result = 0;
cleanup:
    if (result) fprintf(stderr, "ICD compute acceptance failed: %s\n", stage);
    if (submitted) (void)queue_wait_idle(queue);
    if (fence) { destroy_fence(device, fence, NULL); fence = NULL; }
    if (command_pool) { destroy_command_pool(device, command_pool, NULL); command_pool = NULL; }
    if (pipeline) { destroy_pipeline(device, pipeline, NULL); pipeline = NULL; }
    if (descriptor_pool) { destroy_descriptor_pool(device, descriptor_pool, NULL); descriptor_pool = NULL; }
    if (pipeline_layout) { destroy_pipeline_layout(device, pipeline_layout, NULL); pipeline_layout = NULL; }
    if (set_layout) { destroy_descriptor_set_layout(device, set_layout, NULL); set_layout = NULL; }
    if (shader) { destroy_shader_module(device, shader, NULL); shader = NULL; }
    if (mapped) { unmap_memory(device, allocation); mapped = NULL; }
    if (buffer) { destroy_buffer(device, buffer, NULL); buffer = NULL; }
    if (allocation) { free_memory(device, allocation, NULL); allocation = NULL; }
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
    const char *image_stage = NULL;
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
            if (!iteration && !index) {
                printf("ICD production acceptance device: %s\n", property_value.deviceName);
                fflush(stdout);
            }
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
        if (!family_count || family_count > 64) goto fail;
        uint32_t family = 0;
        while (family < family_count && (!families[family].queueCount ||
            (selected_workload == ComputeWorkload &&
             !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))))
            family++;
        if (family == family_count) goto fail;
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
        PFN_vkCreateSemaphore create_semaphore =
            (PFN_vkCreateSemaphore)device_proc(device, "vkCreateSemaphore");
        PFN_vkDestroySemaphore destroy_semaphore =
            (PFN_vkDestroySemaphore)device_proc(device, "vkDestroySemaphore");
        if (!create_semaphore || !destroy_semaphore) goto fail;
        const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = NULL;
        if (create_semaphore(device, &semaphore_info, NULL, &semaphore) != VK_SUCCESS || !semaphore)
            goto fail;
        destroy_semaphore(device, semaphore, NULL);
        PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)device_proc(device, "vkAllocateMemory");
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)device_proc(device, "vkFreeMemory");
        VkPhysicalDeviceMemoryProperties supported_memory = {0};
        memory(devices[0], &supported_memory);
        if (!allocate || !release || !supported_memory.memoryTypeCount ||
            supported_memory.memoryTypeCount > VK_MAX_MEMORY_TYPES) goto fail;
        if (selected_workload == FullWorkload || selected_workload == MappingWorkload) {
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
            PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)device_proc(device, "vkBindBufferMemory");
            if (!bind) goto fail;
            uint32_t memory_type = 0;
            while (memory_type < supported_memory.memoryTypeCount &&
                   (!(buffer_memory.memoryTypeBits & (1u << memory_type)) ||
                    !(supported_memory.memoryTypes[memory_type].propertyFlags &
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                memory_type++;
            if (memory_type == supported_memory.memoryTypeCount) goto fail;
            VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = buffer_memory.size, .memoryTypeIndex = memory_type};
            VkDeviceMemory buffer_allocation = NULL;
            if (allocate(device, &allocation, NULL, &buffer_allocation) != VK_SUCCESS ||
                !buffer_allocation || bind(device, buffer, buffer_allocation, 0) != VK_SUCCESS ||
                device_idle(device) != VK_SUCCESS) goto fail;
            PFN_vkMapMemory map_memory = (PFN_vkMapMemory)device_proc(device, "vkMapMemory");
            PFN_vkUnmapMemory unmap_memory = (PFN_vkUnmapMemory)device_proc(device, "vkUnmapMemory");
            PFN_vkFlushMappedMemoryRanges flush_memory =
                (PFN_vkFlushMappedMemoryRanges)device_proc(device, "vkFlushMappedMemoryRanges");
            PFN_vkInvalidateMappedMemoryRanges invalidate_memory =
                (PFN_vkInvalidateMappedMemoryRanges)device_proc(device, "vkInvalidateMappedMemoryRanges");
            void *mapped = NULL;
            if (!map_memory || !unmap_memory || !flush_memory || !invalidate_memory ||
                map_memory(device, buffer_allocation, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
                !mapped) goto fail;
            const VkMappedMemoryRange mapped_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = buffer_allocation, .size = VK_WHOLE_SIZE};
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
            PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)device_proc(device, "vkQueueSubmit");
            PFN_vkCreateSemaphore create_signal = (PFN_vkCreateSemaphore)device_proc(device, "vkCreateSemaphore");
            PFN_vkDestroySemaphore destroy_signal = (PFN_vkDestroySemaphore)device_proc(device, "vkDestroySemaphore");
            const VkSemaphoreCreateInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore signal = NULL;
            fence_info.flags = 0; fence = NULL;
            if (!submit || !create_signal || !destroy_signal ||
                create_signal(device, &signal_info, NULL, &signal) != VK_SUCCESS || !signal ||
                create_fence(device, &fence_info, NULL, &fence) != VK_SUCCESS || !fence ||
                end_buffer(commands[0]) != VK_SUCCESS) goto fail;
            const VkSubmitInfo signal_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = commands,
                .signalSemaphoreCount = 1, .pSignalSemaphores = &signal};
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            const VkSubmitInfo wait_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &signal, .pWaitDstStageMask = &wait_stage};
            if (submit(queue, 1, &signal_submit, NULL) != VK_SUCCESS ||
                submit(queue, 1, &wait_submit, fence) != VK_SUCCESS ||
                wait_fences(device, 1, &fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
                queue_idle(queue) != VK_SUCCESS) goto fail;
            /* Compare receiver storage, rather than trusting fence retirement alone. */
            if (invalidate_memory(device, 1, &mapped_range) != VK_SUCCESS ||
                memcmp(mapped, update_data, sizeof(update_data))) goto fail;
            unsigned char upload[4096];
            for (unsigned index = 0; index < sizeof(upload); index++)
                upload[index] = (unsigned char)(index * 17 + iteration);
            memcpy(mapped, upload, sizeof(upload));
            memset((unsigned char *)mapped + 32768, 0, sizeof(upload));
            if (flush_memory(device, 1, &mapped_range) != VK_SUCCESS ||
                reset_fences(device, 1, &fence) != VK_SUCCESS ||
                begin_buffer(commands[1], &begin_info) != VK_SUCCESS) goto fail;
            memory_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            pipeline_barrier(commands[1], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &memory_barrier, 0, NULL, 0, NULL);
            const VkBufferCopy upload_copy = {.srcOffset = 0, .dstOffset = 32768, .size = sizeof(upload)};
            copy_buffer(commands[1], buffer, buffer, 1, &upload_copy);
            memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            pipeline_barrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &memory_barrier, 0, NULL, 0, NULL);
            const VkSubmitInfo upload_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = &commands[1]};
            if (end_buffer(commands[1]) != VK_SUCCESS ||
                submit(queue, 1, &upload_submit, fence) != VK_SUCCESS ||
                wait_fences(device, 1, &fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
                invalidate_memory(device, 1, &mapped_range) != VK_SUCCESS ||
                memcmp(mapped, upload, sizeof(upload)) ||
                memcmp((unsigned char *)mapped + 32768, upload, sizeof(upload))) goto fail;
            unmap_memory(device, buffer_allocation);
            mapped = NULL;
            destroy_signal(device, signal, NULL); destroy_fence(device, fence, NULL);
            if (reset_buffer(commands[0], 0) != VK_SUCCESS ||
                reset_buffer(commands[1], 0) != VK_SUCCESS ||
                begin_buffer(commands[1], &begin_info) != VK_SUCCESS ||
                end_buffer(commands[1]) != VK_SUCCESS || reset_pool(device, pool, 0) != VK_SUCCESS)
                goto fail;
            free_buffers(device, pool, 1, commands);
            /* Pool destruction implicitly retires the second loader-dispatchable buffer. */
            destroy_pool(device, pool, NULL);
            destroy_buffer(device, buffer, NULL);
            release(device, buffer_allocation, NULL);

        }

        if (selected_workload == FullWorkload || selected_workload == ImageWorkload) {
            /* Exercise actual receiver image and view ownership independently of
             * mapped transfer storage. Destruction must retire each dependency. */
            PFN_vkCreateImage create_image = (PFN_vkCreateImage)device_proc(device, "vkCreateImage");
            PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)device_proc(device, "vkDestroyImage");
            PFN_vkGetImageMemoryRequirements image_requirements =
                (PFN_vkGetImageMemoryRequirements)device_proc(device, "vkGetImageMemoryRequirements");
            PFN_vkBindImageMemory bind_image = (PFN_vkBindImageMemory)device_proc(device, "vkBindImageMemory");
            PFN_vkCreateImageView create_view =
                (PFN_vkCreateImageView)device_proc(device, "vkCreateImageView");
            PFN_vkDestroyImageView destroy_view =
                (PFN_vkDestroyImageView)device_proc(device, "vkDestroyImageView");
            image_stage = "entry-point lookup";
            if (!create_image || !destroy_image || !image_requirements || !bind_image ||
                !create_view || !destroy_view) goto fail;
            const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
            VkImage image = NULL;
            image_stage = "image creation";
            if (create_image(device, &image_info, NULL, &image) != VK_SUCCESS || !image) goto fail;
            VkMemoryRequirements image_memory = {0};
            image_stage = "memory requirements";
            image_requirements(device, image, &image_memory);
            if (!image_memory.size || !image_memory.alignment || !image_memory.memoryTypeBits) goto fail;
            uint32_t image_type = 0;
            while (image_type < supported_memory.memoryTypeCount &&
                   !(image_memory.memoryTypeBits & (1u << image_type)))
                image_type++;
            if (image_type == supported_memory.memoryTypeCount) goto fail;
            const VkMemoryAllocateInfo image_allocation_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = image_memory.size, .memoryTypeIndex = image_type};
            VkDeviceMemory image_allocation = NULL;
            image_stage = "allocation and bind";
            if (allocate(device, &image_allocation_info, NULL, &image_allocation) != VK_SUCCESS ||
                !image_allocation || bind_image(device, image, image_allocation, 0) != VK_SUCCESS)
                goto fail;
            const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_info.format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1, .layerCount = 1}};
            VkImageView image_view = NULL;
            image_stage = "view creation";
            if (create_view(device, &view_info, NULL, &image_view) != VK_SUCCESS || !image_view)
                goto fail;
#ifdef VgpuIcdLoader
            if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto fail;
#endif
            image_stage = "image barrier entry-point lookup";
            PFN_vkCreateCommandPool create_image_pool =
                (PFN_vkCreateCommandPool)device_proc(device, "vkCreateCommandPool");
            PFN_vkDestroyCommandPool destroy_image_pool =
                (PFN_vkDestroyCommandPool)device_proc(device, "vkDestroyCommandPool");
            PFN_vkAllocateCommandBuffers allocate_image_commands =
                (PFN_vkAllocateCommandBuffers)device_proc(device, "vkAllocateCommandBuffers");
            PFN_vkBeginCommandBuffer begin_image_command =
                (PFN_vkBeginCommandBuffer)device_proc(device, "vkBeginCommandBuffer");
            PFN_vkEndCommandBuffer end_image_command =
                (PFN_vkEndCommandBuffer)device_proc(device, "vkEndCommandBuffer");
            PFN_vkCmdPipelineBarrier image_barrier =
                (PFN_vkCmdPipelineBarrier)device_proc(device, "vkCmdPipelineBarrier");
            PFN_vkQueueSubmit submit_image =
                (PFN_vkQueueSubmit)device_proc(device, "vkQueueSubmit");
            if (!create_image_pool || !destroy_image_pool || !allocate_image_commands ||
                !begin_image_command || !end_image_command || !image_barrier || !submit_image)
                goto fail;
            image_stage = "image command acquisition";
            const VkCommandPoolCreateInfo image_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = family};
            VkCommandPool image_pool = NULL;
            if (create_image_pool(device, &image_pool_info, NULL, &image_pool) != VK_SUCCESS ||
                !image_pool) goto fail;
            const VkCommandBufferAllocateInfo image_command_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = image_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            const VkCommandBufferBeginInfo image_begin_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VkCommandBuffer image_command = NULL;
            if (allocate_image_commands(device, &image_command_info, &image_command) != VK_SUCCESS ||
                !image_command || begin_image_command(image_command, &image_begin_info) != VK_SUCCESS)
                goto fail;
            image_stage = "image layout transition";
            const VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                .subresourceRange = view_info.subresourceRange};
            image_barrier(image_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
            const VkFenceCreateInfo image_fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence image_fence = NULL;
            const VkSubmitInfo image_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = &image_command};
            image_stage = "image submission and fence retirement";
            if (end_image_command(image_command) != VK_SUCCESS ||
                create_fence(device, &image_fence_info, NULL, &image_fence) != VK_SUCCESS || !image_fence ||
                submit_image(queue, 1, &image_submit, image_fence) != VK_SUCCESS ||
                wait_fences(device, 1, &image_fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
                queue_idle(queue) != VK_SUCCESS)
                goto fail;
            destroy_fence(device, image_fence, NULL);
            destroy_image_pool(device, image_pool, NULL);
            destroy_view(device, image_view, NULL);
            destroy_image(device, image, NULL);
            release(device, image_allocation, NULL);
            image_stage = "device teardown after image release";

        }

        if (selected_workload == ComputeWorkload &&
            compute_probe(device, queue, family, &supported_memory, device_proc)) goto fail;

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
        image_stage = NULL;
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
    if (image_stage) fprintf(stderr, "ICD image acceptance failed: %s\n", image_stage);
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
    const char *workload = getenv("WADDLE_TEST_WORKLOAD");
    if (workload && strcmp(workload, "full")) {
        if (!strcmp(workload, "mapping")) selected_workload = MappingWorkload;
        else if (!strcmp(workload, "image")) selected_workload = ImageWorkload;
        else if (!strcmp(workload, "compute")) selected_workload = ComputeWorkload;
        else {
            fputs("Unknown WADDLE_TEST_WORKLOAD; expected full, mapping, image or compute\n", stderr);
            return 2;
        }
    }
    /* Sixteen full-allocation transfer cycles intentionally fragment over the
     * 64-byte stress ring; keep a finite watchdog above that added workload. */
    alarm(300);
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
