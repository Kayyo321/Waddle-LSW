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
#include "shaders/compute_push_shader.h"
#include "shaders/triangle_vertex_shader.h"
#include "shaders/triangle_fragment_shader.h"
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
    ComputeWorkload, /**< Actual shader dispatch and exact mapped output comparison. */
    ComputePushWorkload, /**< Actual distinct runtime push bias and shader result proof. */
    TriangleWorkload /**< Real graphics rendering and noncoherent RGBA readback. */
} fixture_workload_t;
static fixture_workload_t selected_workload = FullWorkload;
static venus_ring_status_t (*icd_bind)(venus_command_exchange_t, void *, const venus_capabilities_t *) = venus_icd_bind_capabilities;
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
    icd_bind = venus_icd_bind_capabilities; icd_unbind = venus_icd_unbind;
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
    return loader_symbol(icd_library, "venus_icd_bind_capabilities", &icd_bind, sizeof(icd_bind)) ||
        loader_symbol(icd_library, "venus_icd_unbind", &icd_unbind, sizeof(icd_unbind)) ||
        loader_symbol(icd_library, "venus_icd_abandon", &icd_abandon, sizeof(icd_abandon)) ||
        loader_symbol(loader_library, "vkGetInstanceProcAddr", &icd_lookup, sizeof(icd_lookup));
}
#endif
/** @brief Sole fixture thread counts real core/Features2 submissions, no retained bytes. */
static uint32_t feature_query_commands;
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
    if (request->kind == RequestSubmit && input && length >= 40) {
        const unsigned char *bytes = input;
        uint32_t command = (uint32_t)bytes[36] | ((uint32_t)bytes[37] << 8) |
            ((uint32_t)bytes[38] << 16) | ((uint32_t)bytes[39] << 24);
        if (command == 3 || command == 147) feature_query_commands++;
    }
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
    if (!function) fprintf(stderr, "ICD production acceptance missing entry point: %s\n", name);
    return function;
}
/** @brief Execute actual storage-buffer shader and compare every result word.
 * @param[in] device Borrowed live device, retained by caller until return.
 * @param[in] queue Borrowed compute-capable queue of family.
 * @param[in] family Existing queue family index.
 * @param[in] supported_memory Borrowed queried actual guest memory properties.
 * @param[in] device_proc Nonnull live device dispatch lookup.
 * @param[in] use_push Nonzero selects the runtime push shader, zero the fixed7 shader.
 * @param[in] bias Runtime u32 bias copied by recording; only used when use_push is nonzero.
 * @return 0 on exact64-word GPU proof,1 on failure; single-threaded fixture.
 * Local resource owner is this function; cleanup releases every acquired resource.
 * supported_memory has1..VK_MAX_MEMORY_TYPES validated records.
 */
static int compute_probe(VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *supported_memory, PFN_vkGetDeviceProcAddr device_proc,
    int use_push, uint32_t bias) {
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
    PFN_vkCmdPushConstants cmd_push_constants = NULL;
    if (use_push) {
        cmd_push_constants = (PFN_vkCmdPushConstants)compute_proc(device, device_proc, "vkCmdPushConstants");
        if (!cmd_push_constants) return 1;
    }
    const uint32_t expected_bias = use_push ? bias : 7;
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
        .codeSize = use_push ? sizeof(ComputePushShader) : sizeof(ComputeShader),
        .pCode = use_push ? ComputePushShader : ComputeShader};
    if (create_shader_module(device, &shader_info, NULL, &shader) != VK_SUCCESS || !shader) goto cleanup;
    const VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo set_layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    if (create_descriptor_set_layout(device, &set_layout_info, NULL, &set_layout) != VK_SUCCESS || !set_layout) goto cleanup;
    const VkPushConstantRange push_range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4};
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = use_push ? 1 : 0,
        .pPushConstantRanges = use_push ? &push_range : NULL};
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
    if (use_push) {
        uint32_t pushed_bias = bias;
        cmd_push_constants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pushed_bias);
        /* Force mutation after synchronous recording: retaining this pointer
         * instead of copied bytes must produce wrong GPU output and fail. */
        *(volatile uint32_t *)&pushed_bias = UINT32_C(0xa5a5a5a5);
    }
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
        if (words[index] != index * 13 + expected_bias) {
            fprintf(stderr, "ICD compute output word %u: got 0x%08x, expected 0x%08x\n",
                index, words[index], index * 13 + expected_bias);
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

/** @brief Render owned red triangle over blue and verify noncoherent readback.
 * @param[in] device Borrowed live device, retained by caller through return.
 * @param[in] queue Borrowed graphics-capable queue belonging to family.
 * @param[in] family Existing graphics queue family index.
 * @param[in] supported_memory Nonnull borrowed queried properties with validated counts.
 * @param[in] device_proc Nonnull borrowed live device dispatch lookup.
 * @return 0 on exact center/corner RGBA bytes, 1 on acquisition or GPU failure.
 * @details Single-threaded. This function owns every acquired local resource;
 * cleanup waits submitted work then releases dependencies before their owners.
 * No borrowed arrays survive the synchronous recording calls.
 */
static int triangle_probe(VkDevice device, VkQueue queue, uint32_t family,
    const VkPhysicalDeviceMemoryProperties *supported_memory, PFN_vkGetDeviceProcAddr device_proc) {
    PFN_vkCreateImage create_image = (PFN_vkCreateImage)compute_proc(device, device_proc, "vkCreateImage");
    if (!create_image) return 1;
    PFN_vkDestroyImage destroy_image = (PFN_vkDestroyImage)compute_proc(device, device_proc, "vkDestroyImage");
    if (!destroy_image) return 1;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = (PFN_vkGetImageMemoryRequirements)compute_proc(device, device_proc, "vkGetImageMemoryRequirements");
    if (!get_image_memory_requirements) return 1;
    PFN_vkBindImageMemory bind_image_memory = (PFN_vkBindImageMemory)compute_proc(device, device_proc, "vkBindImageMemory");
    if (!bind_image_memory) return 1;
    PFN_vkCreateImageView create_image_view = (PFN_vkCreateImageView)compute_proc(device, device_proc, "vkCreateImageView");
    if (!create_image_view) return 1;
    PFN_vkDestroyImageView destroy_image_view = (PFN_vkDestroyImageView)compute_proc(device, device_proc, "vkDestroyImageView");
    if (!destroy_image_view) return 1;
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)compute_proc(device, device_proc, "vkCreateBuffer");
    if (!create_buffer) return 1;
    PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)compute_proc(device, device_proc, "vkDestroyBuffer");
    if (!destroy_buffer) return 1;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = (PFN_vkGetBufferMemoryRequirements)compute_proc(device, device_proc, "vkGetBufferMemoryRequirements");
    if (!get_buffer_memory_requirements) return 1;
    PFN_vkBindBufferMemory bind_buffer_memory = (PFN_vkBindBufferMemory)compute_proc(device, device_proc, "vkBindBufferMemory");
    if (!bind_buffer_memory) return 1;
    PFN_vkAllocateMemory allocate_memory = (PFN_vkAllocateMemory)compute_proc(device, device_proc, "vkAllocateMemory");
    if (!allocate_memory) return 1;
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)compute_proc(device, device_proc, "vkFreeMemory");
    if (!free_memory) return 1;
    PFN_vkMapMemory map_memory = (PFN_vkMapMemory)compute_proc(device, device_proc, "vkMapMemory");
    if (!map_memory) return 1;
    PFN_vkUnmapMemory unmap_memory = (PFN_vkUnmapMemory)compute_proc(device, device_proc, "vkUnmapMemory");
    if (!unmap_memory) return 1;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = (PFN_vkFlushMappedMemoryRanges)compute_proc(device, device_proc, "vkFlushMappedMemoryRanges");
    if (!flush_mapped_memory_ranges) return 1;
    PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = (PFN_vkInvalidateMappedMemoryRanges)compute_proc(device, device_proc, "vkInvalidateMappedMemoryRanges");
    if (!invalidate_mapped_memory_ranges) return 1;
    PFN_vkCreateShaderModule create_shader_module = (PFN_vkCreateShaderModule)compute_proc(device, device_proc, "vkCreateShaderModule");
    if (!create_shader_module) return 1;
    PFN_vkDestroyShaderModule destroy_shader_module = (PFN_vkDestroyShaderModule)compute_proc(device, device_proc, "vkDestroyShaderModule");
    if (!destroy_shader_module) return 1;
    PFN_vkCreatePipelineLayout create_pipeline_layout = (PFN_vkCreatePipelineLayout)compute_proc(device, device_proc, "vkCreatePipelineLayout");
    if (!create_pipeline_layout) return 1;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = (PFN_vkDestroyPipelineLayout)compute_proc(device, device_proc, "vkDestroyPipelineLayout");
    if (!destroy_pipeline_layout) return 1;
    PFN_vkCreateRenderPass create_render_pass = (PFN_vkCreateRenderPass)compute_proc(device, device_proc, "vkCreateRenderPass");
    if (!create_render_pass) return 1;
    PFN_vkDestroyRenderPass destroy_render_pass = (PFN_vkDestroyRenderPass)compute_proc(device, device_proc, "vkDestroyRenderPass");
    if (!destroy_render_pass) return 1;
    PFN_vkCreateFramebuffer create_framebuffer = (PFN_vkCreateFramebuffer)compute_proc(device, device_proc, "vkCreateFramebuffer");
    if (!create_framebuffer) return 1;
    PFN_vkDestroyFramebuffer destroy_framebuffer = (PFN_vkDestroyFramebuffer)compute_proc(device, device_proc, "vkDestroyFramebuffer");
    if (!destroy_framebuffer) return 1;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)compute_proc(device, device_proc, "vkCreateGraphicsPipelines");
    if (!create_graphics_pipelines) return 1;
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
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = (PFN_vkCmdBeginRenderPass)compute_proc(device, device_proc, "vkCmdBeginRenderPass");
    if (!cmd_begin_render_pass) return 1;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = (PFN_vkCmdEndRenderPass)compute_proc(device, device_proc, "vkCmdEndRenderPass");
    if (!cmd_end_render_pass) return 1;
    PFN_vkCmdDraw cmd_draw = (PFN_vkCmdDraw)compute_proc(device, device_proc, "vkCmdDraw");
    if (!cmd_draw) return 1;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = (PFN_vkCmdPipelineBarrier)compute_proc(device, device_proc, "vkCmdPipelineBarrier");
    if (!cmd_pipeline_barrier) return 1;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = (PFN_vkCmdCopyImageToBuffer)compute_proc(device, device_proc, "vkCmdCopyImageToBuffer");
    if (!cmd_copy_image_to_buffer) return 1;
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
    const char *stage = "image acquisition";
    VkImage image = NULL;
    VkDeviceMemory image_memory = NULL, buffer_memory = NULL;
    VkImageView view = NULL;
    VkBuffer buffer = NULL;
    void *mapped = NULL;
    VkShaderModule vertex = NULL, fragment = NULL;
    VkPipelineLayout pipeline_layout = NULL;
    VkRenderPass render_pass = NULL;
    VkFramebuffer framebuffer = NULL;
    VkPipeline pipeline = NULL;
    VkCommandPool command_pool = NULL;
    VkFence fence = NULL;
    const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (create_image(device, &image_info, NULL, &image) != VK_SUCCESS || !image) goto cleanup;
    VkMemoryRequirements image_requirements = {0};
    get_image_memory_requirements(device, image, &image_requirements);
    uint32_t image_type = VK_MAX_MEMORY_TYPES;
    for (uint32_t index = 0; index < supported_memory->memoryTypeCount; index++) {
        if (image_requirements.memoryTypeBits & (UINT32_C(1) << index)) { image_type = index; break; }
    }
    if (!image_requirements.size || image_type == VK_MAX_MEMORY_TYPES) goto cleanup;
    const VkMemoryAllocateInfo image_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size, .memoryTypeIndex = image_type};
    if (allocate_memory(device, &image_allocation, NULL, &image_memory) != VK_SUCCESS || !image_memory ||
        bind_image_memory(device, image, image_memory, 0) != VK_SUCCESS) goto cleanup;
    const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
    if (create_image_view(device, &view_info, NULL, &view) != VK_SUCCESS || !view) goto cleanup;
    stage = "noncoherent readback buffer acquisition";
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16384, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (create_buffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS || !buffer) goto cleanup;
    VkMemoryRequirements buffer_requirements = {0};
    get_buffer_memory_requirements(device, buffer, &buffer_requirements);
    uint32_t buffer_type = VK_MAX_MEMORY_TYPES;
    for (uint32_t index = 0; index < supported_memory->memoryTypeCount; index++) {
        const VkMemoryPropertyFlags flags = supported_memory->memoryTypes[index].propertyFlags;
        if ((buffer_requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            buffer_type = index; break;
        }
    }
    if (buffer_requirements.size < 16384 || buffer_type == VK_MAX_MEMORY_TYPES) goto cleanup;
    const VkMemoryAllocateInfo buffer_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size, .memoryTypeIndex = buffer_type};
    if (allocate_memory(device, &buffer_allocation, NULL, &buffer_memory) != VK_SUCCESS || !buffer_memory ||
        bind_buffer_memory(device, buffer, buffer_memory, 0) != VK_SUCCESS ||
        map_memory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) goto cleanup;
    memset(mapped, 0xa5, 16384);
    const VkMappedMemoryRange mapped_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer_memory, .size = VK_WHOLE_SIZE};
    stage = "triangle mapped poison and flush";
    if (flush_mapped_memory_ranges(device, 1, &mapped_range) != VK_SUCCESS) goto cleanup;
#ifdef VgpuIcdLoader
    if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto cleanup;
#endif
    stage = "shader and render pass acquisition";
    const VkShaderModuleCreateInfo vertex_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(TriangleVertexShader), .pCode = TriangleVertexShader};
    const VkShaderModuleCreateInfo fragment_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(TriangleFragmentShader), .pCode = TriangleFragmentShader};
    if (create_shader_module(device, &vertex_info, NULL, &vertex) != VK_SUCCESS || !vertex ||
        create_shader_module(device, &fragment_info, NULL, &fragment) != VK_SUCCESS || !fragment) goto cleanup;
    const VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (create_pipeline_layout(device, &layout_info, NULL, &pipeline_layout) != VK_SUCCESS || !pipeline_layout) goto cleanup;
    const VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkAttachmentReference color = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    const VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    if (create_render_pass(device, &pass_info, NULL, &render_pass) != VK_SUCCESS || !render_pass) goto cleanup;
    const VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass, .attachmentCount = 1, .pAttachments = &view, .width = 64, .height = 64, .layers = 1};
    if (create_framebuffer(device, &framebuffer_info, NULL, &framebuffer) != VK_SUCCESS || !framebuffer) goto cleanup;
    stage = "graphics pipeline acquisition";
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment, .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkViewport viewport = {.width = 64, .height = 64, .maxDepth = 1};
    const VkRect2D scissor = {.extent = {64, 64}};
    const VkPipelineViewportStateCreateInfo viewport_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const VkPipelineRasterizationStateCreateInfo rasterization = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state, .pRasterizationState = &rasterization, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = render_pass, .basePipelineIndex = -1};
    if (create_graphics_pipelines(device, NULL, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS || !pipeline) goto cleanup;
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    if (create_command_pool(device, &pool_info, NULL, &command_pool) != VK_SUCCESS || !command_pool) goto cleanup;
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command = NULL;
    if (allocate_command_buffers(device, &command_info, &command) != VK_SUCCESS || !command) goto cleanup;
    const VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (begin_command_buffer(command, &begin_info) != VK_SUCCESS) goto cleanup;
    stage = "triangle command recording";
    const VkClearValue clear = {.color = {.float32 = {0, 0, 1, 1}}};
    const VkRenderPassBeginInfo pass_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass, .framebuffer = framebuffer, .renderArea = {.extent = {64, 64}},
        .clearValueCount = 1, .pClearValues = &clear};
    cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    cmd_begin_render_pass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    cmd_draw(command, 3, 1, 0, 0);
    cmd_end_render_pass(command);
    const VkImageMemoryBarrier image_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
    cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_barrier);
    const VkBufferImageCopy region = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent = {64, 64, 1}};
    cmd_copy_image_to_buffer(command, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    const VkBufferMemoryBarrier buffer_barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .size = 16384};
    cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &buffer_barrier, 0, NULL);
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (end_command_buffer(command) != VK_SUCCESS ||
        create_fence(device, &fence_info, NULL, &fence) != VK_SUCCESS || !fence) goto cleanup;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    stage = "triangle submission";
    if (queue_submit(queue, 1, &submit, fence) != VK_SUCCESS) goto cleanup;
    submitted = 1;
    if (wait_for_fences(device, 1, &fence, VK_TRUE, UINT64_C(1000000000)) != VK_SUCCESS ||
        queue_wait_idle(queue) != VK_SUCCESS) goto cleanup;
    submitted = 0;
    stage = "triangle mapped RGBA comparison";
    if (invalidate_mapped_memory_ranges(device, 1, &mapped_range) != VK_SUCCESS) goto cleanup;
    const unsigned char *pixels = mapped;
    const size_t sample_offsets[2] = {(32 * 64 + 32) * 4, 0};
    const unsigned char expected[2][4] = {{255, 0, 0, 255}, {0, 0, 255, 255}};
    for (size_t sample = 0; sample < 2; sample++) {
        if (memcmp(pixels + sample_offsets[sample], expected[sample], 4)) {
            fprintf(stderr, "ICD triangle sample %zu: got [%u,%u,%u,%u], expected [%u,%u,%u,%u]\n", sample,
                pixels[sample_offsets[sample]], pixels[sample_offsets[sample] + 1],
                pixels[sample_offsets[sample] + 2], pixels[sample_offsets[sample] + 3],
                expected[sample][0], expected[sample][1], expected[sample][2], expected[sample][3]);
            goto cleanup;
        }
    }
    result = 0;
cleanup:
    if (result) fprintf(stderr, "ICD triangle acceptance failed: %s\n", stage);
    if (submitted) (void)queue_wait_idle(queue);
    if (fence) { destroy_fence(device, fence, NULL); fence = NULL; }
    if (command_pool) { destroy_command_pool(device, command_pool, NULL); command_pool = NULL; }
    if (pipeline) { destroy_pipeline(device, pipeline, NULL); pipeline = NULL; }
    if (framebuffer) { destroy_framebuffer(device, framebuffer, NULL); framebuffer = NULL; }
    if (render_pass) { destroy_render_pass(device, render_pass, NULL); render_pass = NULL; }
    if (pipeline_layout) { destroy_pipeline_layout(device, pipeline_layout, NULL); pipeline_layout = NULL; }
    if (fragment) { destroy_shader_module(device, fragment, NULL); fragment = NULL; }
    if (vertex) { destroy_shader_module(device, vertex, NULL); vertex = NULL; }
    if (mapped) { unmap_memory(device, buffer_memory); mapped = NULL; }
    if (buffer) { destroy_buffer(device, buffer, NULL); buffer = NULL; }
    if (buffer_memory) { free_memory(device, buffer_memory, NULL); buffer_memory = NULL; }
    if (view) { destroy_image_view(device, view, NULL); view = NULL; }
    if (image) { destroy_image(device, image, NULL); image = NULL; }
    if (image_memory) { free_memory(device, image_memory, NULL); image_memory = NULL; }
    return result;
}

static int icd_cycles(venus_guest_t *guest, int corrupt) {
#ifdef VgpuIcdLoader
    /* Host receiver is already initialized with its original driver environment. */
    if (loader_initialize()) { loader_cleanup(); return 1; }
#endif
    if (icd_bind(command_exchange, guest, &guest->capabilities) != RingOk)
        return 1;
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)icd_lookup(NULL, "vkCreateInstance");
    VkInstance cleanup_instance = NULL;
    VkDevice cleanup_device = NULL;
    PFN_vkDestroyInstance cleanup_destroy_instance = NULL;
    PFN_vkDestroyDevice cleanup_destroy_device = NULL;
    const char *stage = "instance creation";
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifndef VgpuIcdLoader
    /* The negotiated legacy binding exposes API1.0. Enable its guest KHR query
     * facade explicitly; the core1.1 spelling must stay unavailable. */
    const char *extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
#endif
    unsigned iteration = 0;
    for (; iteration < 8; iteration++) {
        stage = "instance creation";
        VkInstance instance = NULL;
        if (!create || create(&info, NULL, &instance) != VK_SUCCESS || !instance)
            goto fail;
        cleanup_instance = instance;
        cleanup_destroy_instance = (PFN_vkDestroyInstance)icd_lookup(instance, "vkDestroyInstance");
        stage = "physical-device enumeration";
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
        stage = "physical query entry points";
        PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)icd_lookup(
                instance, "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceFeatures features =
            (PFN_vkGetPhysicalDeviceFeatures)icd_lookup(
                instance, "vkGetPhysicalDeviceFeatures");
        PFN_vkGetPhysicalDeviceMemoryProperties memory =
            (PFN_vkGetPhysicalDeviceMemoryProperties)icd_lookup(
                instance, "vkGetPhysicalDeviceMemoryProperties");
        if (!properties || !features || !memory) goto fail;
#ifndef VgpuIcdLoader
        stage = "enabled API1.0 KHR Features2 entry point";
        PFN_vkGetPhysicalDeviceFeatures2 features2 =
            (PFN_vkGetPhysicalDeviceFeatures2)icd_lookup(instance, "vkGetPhysicalDeviceFeatures2KHR");
        PFN_vkGetPhysicalDeviceFeatures2 features2_alias =
            (PFN_vkGetPhysicalDeviceFeatures2)venus_icd_get_physical_proc_addr(
                instance, "vkGetPhysicalDeviceFeatures2KHR");
        if (!features2 || features2_alias != features2 ||
            icd_lookup(instance, "vkGetPhysicalDeviceFeatures2")) goto fail;
#else
        stage = "disabled API1.0 KHR Features2 entry point";
        /* The legal loader API remains 1.0 with no enabled instance extension.
         * KHR lookup must remain hidden; the core1.1 entry is never called here. */
        if (icd_lookup(instance, "vkGetPhysicalDeviceFeatures2KHR")) goto fail;
#endif
        stage = "physical properties and cached features";
        for (uint32_t index = 0; index < count; index++) {
            VkPhysicalDeviceProperties property_value = {0};
            VkPhysicalDeviceFeatures feature_value = {0};
            VkPhysicalDeviceMemoryProperties memory_value = {0};
            properties(devices[index], &property_value);
            uint32_t before_features = feature_query_commands;
            features(devices[index], &feature_value);
            if (feature_query_commands != before_features + 1) goto fail;
            VkPhysicalDeviceFeatures repeated;
            memset(&repeated, 0xa5, sizeof(repeated));
            features(devices[index], &repeated);
            VkPhysicalDeviceFeatures zero_core = {0};
            if (feature_query_commands != before_features + 1 ||
                memcmp(&feature_value, &zero_core, sizeof(zero_core)) ||
                memcmp(&repeated, &feature_value, sizeof(repeated))) goto fail;
            memory(devices[index], &memory_value);
#ifndef VgpuIcdLoader
            /* A real negotiated raw query is already cached by the core getter. Both
             * enabled KHR lookups must publish the same proven intersection without
             * overwriting unknown payloads, caller headers, links or canaries. */
            struct unknown_t { VkStructureType sType; void *pNext; uint64_t sentinel; } unknown;
            memset(&unknown, 0xa5, sizeof(unknown));
            unknown.sType = (VkStructureType)999999; unknown.pNext = NULL;
            VkPhysicalDeviceShaderDrawParametersFeatures draw = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
                .pNext = &unknown, .shaderDrawParameters = VK_TRUE};
            VkPhysicalDeviceRobustness2FeaturesEXT robust = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                .pNext = &draw, .robustBufferAccess2 = VK_TRUE,
                .robustImageAccess2 = VK_TRUE, .nullDescriptor = VK_TRUE};
            VkPhysicalDeviceFeatures2 complete = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &robust};
            memset(&complete.features, 0xa5, sizeof(complete.features));
            features2(devices[index], &complete);
            features2_alias(devices[index], &complete);
            if (feature_query_commands != before_features + 1 ||
                memcmp(&feature_value, &zero_core, sizeof(zero_core)) ||
                memcmp(&complete.features, &feature_value, sizeof(feature_value)) ||
                complete.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 || complete.pNext != &robust ||
                robust.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT || robust.pNext != &draw ||
                robust.robustBufferAccess2 || robust.robustImageAccess2 || robust.nullDescriptor ||
                draw.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES || draw.pNext != &unknown ||
                draw.shaderDrawParameters || unknown.sType != (VkStructureType)999999 || unknown.pNext ||
                unknown.sentinel != UINT64_C(0xa5a5a5a5a5a5a5a5))
                goto fail;
#endif
            if (property_value.apiVersion < VK_API_VERSION_1_0 || !property_value.deviceName[0] ||
                !memory_value.memoryTypeCount || !memory_value.memoryHeapCount)
                goto fail;
            if (!iteration && !index) {
                printf("ICD production acceptance device: %s\n", property_value.deviceName);
                fflush(stdout);
            }
        }
        stage = "queue-family selection";
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
            ((selected_workload == ComputeWorkload || selected_workload == ComputePushWorkload) &&
             !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ||
            (selected_workload == TriangleWorkload && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))))
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
        stage = "device creation";
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
        stage = "core synchronization lifecycle";
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
            stage = "mapped transfer lifecycle";
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
            stage = "image entry-point lookup";
            if (!create_image || !destroy_image || !image_requirements || !bind_image ||
                !create_view || !destroy_view) goto fail;
            const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
            VkImage image = NULL;
            stage = "image creation";
            if (create_image(device, &image_info, NULL, &image) != VK_SUCCESS || !image) goto fail;
            VkMemoryRequirements image_memory = {0};
            stage = "image memory requirements";
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
            stage = "image allocation and bind";
            if (allocate(device, &image_allocation_info, NULL, &image_allocation) != VK_SUCCESS ||
                !image_allocation || bind_image(device, image, image_allocation, 0) != VK_SUCCESS)
                goto fail;
            const VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_info.format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1, .layerCount = 1}};
            VkImageView image_view = NULL;
            stage = "image view creation";
            if (create_view(device, &view_info, NULL, &image_view) != VK_SUCCESS || !image_view)
                goto fail;
#ifdef VgpuIcdLoader
            if (getenv("WADDLE_TEST_LOADER_FAILURE")) goto fail;
#endif
            stage = "image barrier entry-point lookup";
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
            stage = "image command acquisition";
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
            stage = "image layout transition";
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
            stage = "image submission and fence retirement";
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
            stage = "device teardown after image release";

        }

        stage = "compute workload";
        if ((selected_workload == ComputeWorkload || selected_workload == ComputePushWorkload) &&
            compute_probe(device, queue, family, &supported_memory, device_proc,
                selected_workload == ComputePushWorkload, 37 + iteration * 19 + (uint32_t)corrupt * 257))
            goto fail;

        stage = "triangle workload";
        if (selected_workload == TriangleWorkload &&
            triangle_probe(device, queue, family, &supported_memory, device_proc)) goto fail;

        stage = "device and instance teardown";
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
    stage = "empty ICD unbind";
    if (icd_unbind() == RingOk) {
#ifdef VgpuIcdLoader
        loader_cleanup();
#endif
        return 0;
    }
fail:
    fprintf(stderr, "ICD production acceptance failed: stage=%s, cycle=%u, mode=%d\n",
        stage, iteration, corrupt);
    /* Release loader-owned CPU dispatch tables even when host ownership is uncertain. */
    if (cleanup_device && cleanup_destroy_device) cleanup_destroy_device(cleanup_device, NULL);
    if (cleanup_instance && cleanup_destroy_instance) cleanup_destroy_instance(cleanup_instance, NULL);
    venus_guest_free(guest); /* Stop receiver access before forgetting reserved objects. */
    return 1;
}
static int run_fixture(int corrupt) {
    int result = 1, mapping_fd = -1, streams[2] = {-1, -1}, frames[2] = {-1, -1};
    const char *stage = "production worker executable";
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
    stage = "shared mapping acquisition";
    mapping_fd = memfd_create("presented-worker", MFD_CLOEXEC);
    if (mapping_fd < 0 || ftruncate(mapping_fd, 4096) != 0)
        goto cleanup;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, 0);
    if (mapping == MAP_FAILED || venus_region_init(mapping, 4096, 64) != RingOk ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, streams) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, frames) != 0 ||
        venus_frame_prepare(frames[0]) != RingOk || venus_frame_prepare(frames[1]) != RingOk)
        goto cleanup;
    stage = "production worker exec";
    if (venus_worker_create_presented(&worker, executable, mapping_fd, streams[1], frames[1],
                                      UINT64_MAX) != RingOk)
        goto cleanup;
    close(streams[1]);
    streams[1] = -1;
    close(frames[1]);
    frames[1] = -1;
    stage = "channel handshake and capability negotiation";
    if (venus_session_init(&session, SessionGuest, mapping, 4096, 0) != RingOk ||
        venus_channel_init(&channel, &session, streams[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) != RingOk ||
        venus_guest_init(&guest, &rpc, 5000) != RingOk)
        goto cleanup;
    stage = "raw version query";
    if (query_version(&guest)) goto cleanup;
    stage = "raw instance lifecycle";
    if (instance_cycle(&guest)) goto cleanup;
    stage = "ICD lifecycle";
    if (icd_cycles(&guest, corrupt)) goto cleanup;
    stage = "unregistered presentation rejection";
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
    stage = "unknown-release shutdown";
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
    stage = "production worker exit status";
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
    if (venus_worker_destroy(&worker, 1000) != RingOk) {
        if (!result) stage = "production worker retirement";
        result = 1;
    }
    if (result) fprintf(stderr, "Presented worker fixture failed: stage=%s, mode=%d\n", stage, corrupt);
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
        else if (!strcmp(workload, "compute_push")) selected_workload = ComputePushWorkload;
        else if (!strcmp(workload, "triangle")) selected_workload = TriangleWorkload;
        else {
            fputs("Unknown WADDLE_TEST_WORKLOAD; expected full, mapping, image, compute, compute_push or triangle\n", stderr);
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
            fprintf(stderr, "Presented worker binding failed: mode=%u, fixture=%d, descriptors=%u/%u\n",
                corrupt, fixture_result, after, baseline);
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
