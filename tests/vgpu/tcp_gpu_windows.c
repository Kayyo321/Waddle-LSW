/** @file tcp_gpu_windows.c @brief Actual native authenticated offscreen Vulkan acceptance.
 * Probe bodies below are immutable byte-for-byte copies from worker_presented.c
 * SHA256 a68383c92faad311bdbe885fbb7da57622144d08164553469322a63fce953968.
 * No mock exchange/capability/renderer is installed. Public API stays1.0 with no
 * enabled feature. triangle_queries legally enables KHR physical queries; other
 * workloads retain no extensions. Physical acceptance requires independent host proof.
 */
#define COBJMACROS
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <dwmapi.h>
#define VgpuIcdLoader
#include "waddle/venus_tcp.h"
#include "waddle/venus_icd.h"
#include "shaders/compute_shader.h"
#include "shaders/compute_push_shader.h"
#include "shaders/triangle_vertex_shader.h"
#include "shaders/triangle_fragment_shader.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/** @brief Explicit lifecycle ABI; DLL owns its retained session/context, caller owns module. */
typedef venus_ring_status_t (*bootstrap_start_t)(const char *,size_t);
/** @brief Quiescent stop ABI; errors retain ownership, caller must not unload. */
typedef venus_ring_status_t (*bootstrap_stop_t)(void);
/** @brief Copied session identity ABI, no allocation or transfer. */
typedef uint64_t (*bootstrap_session_t)(void);
/** @brief Release ABI after trusted exact receiver retirement only. */
typedef venus_ring_status_t (*bootstrap_abandon_t)(uint64_t);
static int symbol(HMODULE module,const char *name,void *output,size_t bytes)
{
    FARPROC procedure=GetProcAddress(module,name);if(!procedure || bytes!=sizeof procedure)return 1;
    memcpy(output,&procedure,bytes);return 0;
}
static int medium_integrity(void)
{
    HANDLE token=NULL;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 0;
    _Alignas(TOKEN_MANDATORY_LABEL) uint8_t bytes[256];DWORD length=0;int valid=0;
    if(GetTokenInformation(token,TokenIntegrityLevel,bytes,sizeof bytes,&length) && length>=sizeof(TOKEN_MANDATORY_LABEL) && length<=sizeof bytes) {
        TOKEN_MANDATORY_LABEL label={0};memcpy(&label,bytes,sizeof label);
        if(label.Label.Sid && IsValidSid(label.Label.Sid)) {
            UCHAR count=*GetSidSubAuthorityCount(label.Label.Sid);
            valid=count && *GetSidSubAuthority(label.Label.Sid,count-1)==SECURITY_MANDATORY_MEDIUM_RID;
        }
    }
    memset(bytes,0,sizeof bytes);while(!CloseHandle(token))Sleep(10);return valid;
}
static DWORD native_handles(void)
{
    DWORD count=0;if(!GetProcessHandleCount(GetCurrentProcess(),&count))return 0;return count;
}
static int handle_baseline(DWORD baseline)
{
    for(unsigned attempt=0;attempt<100;attempt++){DWORD count=native_handles();if(!count)return 1;if(count==baseline)return 0;Sleep(10);}
    fprintf(stderr,"Native GPU handle baseline=%lu actual=%lu\n",(unsigned long)baseline,(unsigned long)native_handles());return 1;
}
/** @brief Initialize independent Windows process-lifetime graphics dependencies.
 * @param[out] retained_module Nonnull sole module owner, initialized to NULL.
 * @return Zero after factory/adapter references release; caller owns the retained
 * system DXGI reference through application lifetimes, then FreeLibrary closes it.
 * @details The pinned loader initializes system DXGI for device sorting, even
 * with a private ICD override. Preserved cold97->157 traces attribute the extra
 * objects to DXGI/COM/RPC and loader OutputDebugString DBWinMutex, with no Waddle
 * module creation frames. Initialize those same system paths before asserting
 * exact repeated application ownership. This owns no Vulkan device or transport
 * resource and never closes any process-lifetime system handle.
 * Single main thread. Native COM references are local; the transferred module
 * reference prevents unload/reload of module-owned Windows state before the
 * loader takes its own process-lifetime system DXGI reference.
 */
static int initialize_system_graphics(HMODULE *retained_module)
{
    if(!retained_module || *retained_module)return 1;
    DWORD cold=native_handles();if(!cold)return 1;
    HMODULE module=LoadLibraryExA("dxgi.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)return 1;
    typedef HRESULT (WINAPI *create_factory_t)(REFIID,void **);
    FARPROC procedure=GetProcAddress(module,"CreateDXGIFactory1");create_factory_t create=NULL;
    _Static_assert(sizeof create==sizeof procedure,"Windows factory function representation");memcpy(&create,&procedure,sizeof create);
    IDXGIFactory6 *factory=NULL;int result=1;
    if(!create || FAILED(create(&IID_IDXGIFactory6,(void **)&factory)) || !factory)goto cleanup;
    for(UINT index=0;;index++) {
        IDXGIAdapter1 *adapter=NULL;HRESULT status=IDXGIFactory6_EnumAdapterByGpuPreference(factory,index,DXGI_GPU_PREFERENCE_UNSPECIFIED,&IID_IDXGIAdapter1,(void **)&adapter);
        if(status==DXGI_ERROR_NOT_FOUND){result=0;break;}
        if(FAILED(status) || !adapter)break;
        DXGI_ADAPTER_DESC1 description={0};status=IDXGIAdapter1_GetDesc1(adapter,&description);IDXGIAdapter1_Release(adapter);
        if(FAILED(status))break;
    }
cleanup:
    if(factory)IDXGIFactory6_Release(factory);
    if(result){while(!FreeLibrary(module))Sleep(10);return 1;}
    *retained_module=module;
    /* OutputDebugString itself initializes a process-lifetime system mutex.
     * The pinned loader diagnostics use this same OS path. */
    OutputDebugStringA("Waddle acceptance initializes Windows debug output before exact owned-handle baseline.\n");
    /* The loader's package-family query initializes a native StateRepository
     * package index key. The reused trace diagnostic identified this precise
     * remaining system object after independent DXGI/debug initialization. */
    typedef LONG (WINAPI *package_family_t)(PCWSTR,UINT32 *,PWSTR *,UINT32 *,WCHAR *);
    FARPROC package_proc=GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetPackagesByPackageFamily");
    package_family_t package_family=NULL;memcpy(&package_family,&package_proc,sizeof package_family);
    if(!package_family)return 1;
    UINT32 package_count=0,package_bytes=0;
    LONG package_status=package_family(L"Microsoft.D3DMappingLayers_8wekyb3d8bbwe",&package_count,NULL,&package_bytes,NULL);
    if(package_status!=ERROR_SUCCESS && package_status!=ERROR_INSUFFICIENT_BUFFER)return 1;
    Sleep(100);
    printf("Independent Windows DXGI/COM/RPC/debug initialization handles cold=%lu initialized=%lu; factory/adapter references released, caller retains module\n",(unsigned long)cold,(unsigned long)native_handles());fflush(stdout);
    return 0;
}
/** @brief Wait while retaining modules for trusted fixed exact-session receipt.
 * @param[in] path Immutable absolute private supervisor filename, borrowed.
 * @param[in] identity Established copied session, nonzero. @return Only after exact
 * bounded literal match. No untrusted integer parser, allocation or context mutation.
 * Sole fixture thread; supervisor owns private receipt and exact host-retirement proof.
 */
static void wait_retired(const char *path,uint64_t identity)
{
    char expected[64];int length=snprintf(expected,sizeof expected,"retired_session=%016llx\n",(unsigned long long)identity);
    if(length<=0 || (size_t)length>=sizeof expected)abort();
    fprintf(stderr,"Retained native session=%016llx awaiting trusted actual worker retirement\n",(unsigned long long)identity);fflush(stderr);
    for(;;) {
        HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information={0};char actual[65];DWORD count=0;int match=0;
            if(GetFileType(file)==FILE_TYPE_DISK && GetFileInformationByHandle(file,&information) &&
                !(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && information.nNumberOfLinks==1 &&
                !information.nFileSizeHigh && information.nFileSizeLow==(DWORD)length &&
                ReadFile(file,actual,sizeof actual,&count,NULL) && count==(DWORD)length && !memcmp(actual,expected,(size_t)length))match=1;
            if(!CloseHandle(file)){fprintf(stderr,"Receipt handle release failed; retaining owner\n");for(;;)Sleep(10);}
            if(match)return;
        }
        Sleep(10);
    }
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

#include "tcp_gpu_present_windows.inc"

/** @brief Query actual modern physical wire paths without enabling unimplemented features.
 * @param[in] physical Live borrowed device, lookup Live borrowed instance dispatcher,
 * instance Parent lifetime, legacy Actual projected legacy features/properties.
 * @return Zero after legal enabled KHR queries, boolean domains and host properties.
 * @note Sole fixture thread; stack chains borrowed only until synchronous calls return.
 */
static int modern_query_probe(VkPhysicalDevice physical,PFN_vkGetInstanceProcAddr lookup,VkInstance instance,
    const VkPhysicalDeviceFeatures *legacy,const VkPhysicalDeviceProperties *property)
{
    PFN_vkGetPhysicalDeviceFeatures2KHR get_features=(PFN_vkGetPhysicalDeviceFeatures2KHR)lookup(instance,"vkGetPhysicalDeviceFeatures2KHR");
    PFN_vkGetPhysicalDeviceProperties2KHR get_properties=(PFN_vkGetPhysicalDeviceProperties2KHR)lookup(instance,"vkGetPhysicalDeviceProperties2KHR");
    if(!get_features || !get_properties)return 1;
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceRobustness2FeaturesEXT robust={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,.pNext=&maintenance};
    VkPhysicalDeviceVulkan13Features features13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&robust};
    VkPhysicalDeviceVulkan12Features features12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&features13};
    VkPhysicalDeviceVulkan11Features features11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&features12};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&features11};
    get_features(physical,&features);
    if(memcmp(&features.features,legacy,sizeof *legacy) || features.pNext!=&features11 || features11.pNext!=&features12 || features12.pNext!=&features13 || features13.pNext!=&robust || robust.pNext!=&maintenance || maintenance.pNext)return 1;
    const void *nodes[]={&features11,&features12,&features13,&robust,&maintenance};
    const size_t offsets[]={offsetof(VkPhysicalDeviceVulkan11Features,storageBuffer16BitAccess),offsetof(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge),offsetof(VkPhysicalDeviceVulkan13Features,robustImageAccess),offsetof(VkPhysicalDeviceRobustness2FeaturesEXT,robustBufferAccess2),offsetof(VkPhysicalDeviceMaintenance5FeaturesKHR,maintenance5)};
    const size_t counts[]={12,47,15,3,1};
    for(size_t index=0;index<5;index++)for(size_t flag=0;flag<counts[index];flag++){VkBool32 value=0;memcpy(&value,(const uint8_t *)nodes[index]+offsets[index]+flag*sizeof value,sizeof value);if(value>VK_TRUE)return 1;}
    VkPhysicalDeviceVulkan13Properties properties13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties properties12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,.pNext=&properties13};
    VkPhysicalDeviceVulkan11Properties properties11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,.pNext=&properties12};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&properties11};
    get_properties(physical,&properties);
    if(properties.properties.apiVersion!=property->apiVersion || properties.properties.vendorID!=property->vendorID || properties.properties.deviceID!=property->deviceID || strcmp(properties.properties.deviceName,property->deviceName) || !properties13.maxBufferSize || !properties12.driverName[0] || !memchr(properties12.driverName,0,sizeof properties12.driverName) || properties.pNext!=&properties11 || properties11.pNext!=&properties12 || properties12.pNext!=&properties13 || properties13.pNext)return 1;
    PFN_vkGetPhysicalDeviceFormatProperties2KHR get_format=(PFN_vkGetPhysicalDeviceFormatProperties2KHR)lookup(instance,"vkGetPhysicalDeviceFormatProperties2KHR");
    PFN_vkGetPhysicalDeviceImageFormatProperties2KHR get_image=(PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)lookup(instance,"vkGetPhysicalDeviceImageFormatProperties2KHR");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR get_queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR)lookup(instance,"vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    PFN_vkGetPhysicalDeviceMemoryProperties2KHR get_memory=(PFN_vkGetPhysicalDeviceMemoryProperties2KHR)lookup(instance,"vkGetPhysicalDeviceMemoryProperties2KHR");
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR get_sparse=(PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR)lookup(instance,"vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
    if(!get_format || !get_image || !get_queues || !get_memory || !get_sparse)return 1;
    VkFormatProperties2 format={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};get_format(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    if(!(format.formatProperties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))return 1;
    VkPhysicalDeviceImageFormatInfo2 image_info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.format=VK_FORMAT_R8G8B8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImageFormatProperties2 image={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if(get_image(physical,&image_info,&image)!=VK_SUCCESS || image.imageFormatProperties.maxExtent.width<64 || image.imageFormatProperties.maxExtent.height<64 || !(image.imageFormatProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return 1;
    uint32_t queue_count=0;get_queues(physical,&queue_count,NULL);if(!queue_count || queue_count>64)return 1;
    VkQueueFamilyProperties2 queue_properties[64]={0};for(uint32_t index=0;index<queue_count;index++)queue_properties[index].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    uint32_t queue_capacity=queue_count;get_queues(physical,&queue_capacity,queue_properties);if(queue_capacity!=queue_count)return 1;
    int has_graphics=0;for(uint32_t index=0;index<queue_count;index++){if(queue_properties[index].sType!=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 || queue_properties[index].pNext)return 1;if(queue_properties[index].queueFamilyProperties.queueCount && (queue_properties[index].queueFamilyProperties.queueFlags&VK_QUEUE_GRAPHICS_BIT))has_graphics=1;}
    if(!has_graphics)return 1;
    VkPhysicalDeviceMemoryProperties2 memory={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};get_memory(physical,&memory);
    if(!memory.memoryProperties.memoryTypeCount || memory.memoryProperties.memoryTypeCount>VK_MAX_MEMORY_TYPES || !memory.memoryProperties.memoryHeapCount || memory.memoryProperties.memoryHeapCount>VK_MAX_MEMORY_HEAPS)return 1;
    for(uint32_t index=0;index<memory.memoryProperties.memoryTypeCount;index++)if(memory.memoryProperties.memoryTypes[index].heapIndex>=memory.memoryProperties.memoryHeapCount)return 1;
    VkPhysicalDeviceSparseImageFormatInfo2 sparse_info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,.format=VK_FORMAT_R8G8B8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL};
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties legacy_sparse=(PFN_vkGetPhysicalDeviceSparseImageFormatProperties)lookup(instance,"vkGetPhysicalDeviceSparseImageFormatProperties");
    if(!legacy_sparse)return 1;
    uint32_t sparse_count=0,legacy_count=0;get_sparse(physical,&sparse_info,&sparse_count,NULL);
    legacy_sparse(physical,sparse_info.format,sparse_info.type,sparse_info.samples,sparse_info.usage,sparse_info.tiling,&legacy_count,NULL);
    if(sparse_count!=legacy_count || sparse_count>64)return 1;
    VkSparseImageFormatProperties2 sparse_properties[64]={0};VkSparseImageFormatProperties legacy_properties[64]={0};
    for(uint32_t index=0;index<sparse_count;index++)sparse_properties[index].sType=VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
    uint32_t sparse_capacity=sparse_count,legacy_capacity=legacy_count;
    get_sparse(physical,&sparse_info,&sparse_capacity,sparse_properties);
    legacy_sparse(physical,sparse_info.format,sparse_info.type,sparse_info.samples,sparse_info.usage,sparse_info.tiling,&legacy_capacity,legacy_properties);
    if(sparse_capacity!=sparse_count || legacy_capacity!=legacy_count)return 1;
    for(uint32_t index=0;index<sparse_count;index++)if(sparse_properties[index].sType!=VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2 || sparse_properties[index].pNext || memcmp(&sparse_properties[index].properties,&legacy_properties[index],sizeof legacy_properties[index]))return 1;
    printf("Native modern KHR physical queries PASS driver=%s max_buffer=%llu\n",properties12.driverName,(unsigned long long)properties13.maxBufferSize);fflush(stdout);return 0;
}

/** @brief Run eight actual GPU lifetimes over one explicit authenticated binding.
 * @param[in] argc/argv Immutable trusted absolute bootstrap/loader/manifest/config/
 * retirement-receipt paths, workload and exact physical-device name.
 * @return Zero only after exact real workload outputs and matching retirement Ack;
 * nonzero failure, retaining DLL/context until trusted host retirement if necessary.
 * Sole medium-integrity lifecycle thread. No allocation beyond application Vulkan
 * objects; bootstrap and loader references outlive all calls/CPU dispatch tables.
 */
int main(int argc,char **argv)
{
    if(argc!=8 || (strcmp(argv[6],"triangle") && strcmp(argv[6],"compute") && strcmp(argv[6],"compute_push") && strcmp(argv[6],"triangle_queries") && strcmp(argv[6],"triangle_present") && strcmp(argv[6],"triangle_present_smoke")))return 2;
    int modern_queries=!strcmp(argv[6],"triangle_queries");
    int presentation_smoke=!strcmp(argv[6],"triangle_present_smoke");
    int presentation=!strcmp(argv[6],"triangle_present") || presentation_smoke;
    int graphics_workload=!strcmp(argv[6],"triangle") || modern_queries || presentation;
    if(!medium_integrity()){fputs("Native GPU fixture requires medium integrity\n",stderr);return 2;}
    if(GetFileAttributesA(argv[5])!=INVALID_FILE_ATTRIBUTES){fputs("Retirement receipt must be fresh\n",stderr);return 2;}
    HMODULE bootstrap_module=NULL,loader_module=NULL,system_dxgi_module=NULL;
    bootstrap_start_t start=NULL;bootstrap_stop_t stop=NULL;bootstrap_session_t session=NULL;bootstrap_abandon_t abandon=NULL;
    PFN_vkGetInstanceProcAddr lookup=NULL;VkInstance instance=NULL;VkDevice device=NULL;
    PFN_vkDestroyInstance destroy_instance=NULL;PFN_vkDestroyDevice destroy_device=NULL;
    PFN_vkDeviceWaitIdle cleanup_idle=NULL;
    const char *stage="bootstrap module acquisition";int result=1;uint64_t identity=0;
    bootstrap_module=LoadLibraryExA(argv[1],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!bootstrap_module)goto cleanup;
    if(symbol(bootstrap_module,"venus_tcp_bootstrap_start",&start,sizeof start) ||
        symbol(bootstrap_module,"venus_tcp_bootstrap_stop",&stop,sizeof stop) ||
        symbol(bootstrap_module,"venus_tcp_bootstrap_session",&session,sizeof session) ||
        symbol(bootstrap_module,"venus_tcp_bootstrap_abandon",&abandon,sizeof abandon))goto cleanup;
    stage="actual TCP authentication and ICD capability binding";
    if(start(argv[4],strlen(argv[4])+1)!=RingOk)goto cleanup;
    identity=session();if(!identity)goto cleanup;
    printf("Authenticated native GPU session=%016llx workload=%s public_api=1.0\n",(unsigned long long)identity,argv[6]);fflush(stdout);
    stage="private pinned loader discovery";
    if(_putenv_s("VK_DRIVER_FILES",argv[3]) || _putenv_s("VK_ICD_FILENAMES",argv[3]))goto cleanup;
    loader_module=LoadLibraryExA(argv[2],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!loader_module || symbol(loader_module,"vkGetInstanceProcAddr",&lookup,sizeof lookup))goto cleanup;
    PFN_vkCreateInstance create_instance=(PFN_vkCreateInstance)lookup(NULL,"vkCreateInstance");
    PFN_vkDestroyInstance owned_destroy_instance=NULL;PFN_vkDestroyDevice owned_destroy_device=NULL;
    if(!create_instance || symbol(loader_module,"vkDestroyInstance",&owned_destroy_instance,sizeof owned_destroy_instance) ||
        symbol(loader_module,"vkDestroyDevice",&owned_destroy_device,sizeof owned_destroy_device))goto cleanup;
    stage="independent process-lifetime Windows graphics initialization";
    if(initialize_system_graphics(&system_dxgi_module))goto cleanup;
    if(presentation && initialize_presentation_windows())goto cleanup;
    DWORD baseline=native_handles();if(!baseline)goto cleanup;
    printf("Native repeated application handle baseline=%lu\n",(unsigned long)baseline);fflush(stdout);
    for(unsigned iteration=0;iteration<(presentation_smoke ? 2u : 8u);iteration++) {
        stage="Vulkan1.0 instance creation";
        VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_0};
        const char *query_extension=VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
        const char *present_extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application,.enabledExtensionCount=presentation ? 2u : (modern_queries ? 1u : 0u),.ppEnabledExtensionNames=presentation ? present_extensions : (modern_queries ? &query_extension : NULL)};
        if(create_instance(&info,NULL,&instance)!=VK_SUCCESS || !instance)goto cleanup;
        destroy_instance=owned_destroy_instance;
        PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)lookup(instance,"vkEnumeratePhysicalDevices");
        PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)lookup(instance,"vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)lookup(instance,"vkGetPhysicalDeviceFeatures");
        PFN_vkGetPhysicalDeviceMemoryProperties memory=(PFN_vkGetPhysicalDeviceMemoryProperties)lookup(instance,"vkGetPhysicalDeviceMemoryProperties");
        PFN_vkGetPhysicalDeviceQueueFamilyProperties queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)lookup(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
        PFN_vkCreateDevice create_device=(PFN_vkCreateDevice)lookup(instance,"vkCreateDevice");
        PFN_vkGetDeviceProcAddr device_proc=(PFN_vkGetDeviceProcAddr)lookup(instance,"vkGetDeviceProcAddr");
        if(!destroy_instance || !enumerate || !properties || !features || !memory || !queues || !create_device || !device_proc || (!modern_queries && lookup(instance,"vkGetPhysicalDeviceFeatures2KHR")))goto cleanup;
        stage="exact actual NVIDIA selection and zero feature intersection";
        uint32_t count=0;if(enumerate(instance,&count,NULL)!=VK_SUCCESS || !count || count>16)goto cleanup;
        VkPhysicalDevice devices[16]={0};uint32_t capacity=count;if(enumerate(instance,&capacity,devices)!=VK_SUCCESS || capacity!=count)goto cleanup;
        VkPhysicalDevice selected=NULL;VkPhysicalDeviceMemoryProperties supported_memory={0};
        for(uint32_t index=0;index<count;index++) {
            VkPhysicalDeviceProperties property={0};VkPhysicalDeviceFeatures actual={0},zero={0};properties(devices[index],&property);features(devices[index],&actual);
            if(property.apiVersion!=VK_API_VERSION_1_0 || memcmp(&actual,&zero,sizeof zero))goto cleanup;
            if(!strcmp(property.deviceName,argv[7])){if(selected)goto cleanup;selected=devices[index];memory(selected,&supported_memory);if(modern_queries && modern_query_probe(selected,lookup,instance,&actual,&property))goto cleanup;}
        }
        if(!selected || !supported_memory.memoryTypeCount || supported_memory.memoryTypeCount>VK_MAX_MEMORY_TYPES || !supported_memory.memoryHeapCount || supported_memory.memoryHeapCount>VK_MAX_MEMORY_HEAPS)goto cleanup;
        uint32_t family_count=64;VkQueueFamilyProperties families[64]={0};queues(selected,&family_count,families);if(!family_count || family_count>64)goto cleanup;
        VkQueueFlags required=graphics_workload ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT;
        uint32_t family=0;while(family<family_count && (!families[family].queueCount || !(families[family].queueFlags&required)))family++;
        if(family==family_count)goto cleanup;
        float priority=0.5f;VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
        const char *swapchain_extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.enabledExtensionCount=presentation ? 1u : 0u,.ppEnabledExtensionNames=presentation ? &swapchain_extension : NULL};
        destroy_device=owned_destroy_device;
        stage="actual device and queue creation";if(create_device(selected,&device_info,NULL,&device)!=VK_SUCCESS || !device)goto cleanup;
        if(!device_proc(device,"vkDestroyDevice"))goto cleanup;
        PFN_vkGetDeviceQueue get_queue=(PFN_vkGetDeviceQueue)device_proc(device,"vkGetDeviceQueue");
        PFN_vkDeviceWaitIdle device_idle=(PFN_vkDeviceWaitIdle)device_proc(device,"vkDeviceWaitIdle");
        if(!destroy_device || !get_queue || !device_idle)goto cleanup;
        cleanup_idle=device_idle;
        VkQueue queue=NULL,repeated=NULL;get_queue(device,family,0,&queue);get_queue(device,family,0,&repeated);if(!queue || repeated!=queue)goto cleanup;
        stage="exact hardware workload output";
        if(presentation){if(native_swapchain_probe(instance,selected,device,queue,family,&supported_memory,lookup,device_proc,presentation_smoke))goto cleanup;}
        else if(graphics_workload){if(triangle_probe(device,queue,family,&supported_memory,device_proc))goto cleanup;}
        else if(compute_probe(device,queue,family,&supported_memory,device_proc,!strcmp(argv[6],"compute_push"),37+iteration*19))goto cleanup;
        stage="application teardown before matching actual retirement Ack";
        if(device_idle(device)!=VK_SUCCESS)goto cleanup;
        destroy_device(device,NULL);device=NULL;destroy_device=NULL;cleanup_idle=NULL;destroy_instance(instance,NULL);instance=NULL;destroy_instance=NULL;
        stage="exact native application handle retirement";if(handle_baseline(baseline))goto cleanup;
        printf("Native actual %s lifetime=%u device=%s exact-output PASS\n",argv[6],iteration+1,argv[7]);fflush(stdout);
    }
    if(stop()!=RingOk || session())goto cleanup;
    result=0;
cleanup:
    if(result)fprintf(stderr,"Native actual GPU fixture failed: %s\n",stage);
    /* A healthy validation failure can retire its ordinary application owners
     * before Stop. Unknown GPU completion retains them until trusted host reap. */
    if(device && destroy_device && cleanup_idle && cleanup_idle(device)==VK_SUCCESS){destroy_device(device,NULL);device=NULL;destroy_device=NULL;cleanup_idle=NULL;}
    if(!device && instance && destroy_instance){destroy_instance(instance,NULL);instance=NULL;destroy_instance=NULL;}
    if(stop && session && abandon) {
        venus_ring_status_t status=stop();
        uint64_t retained=session();
        if(status!=RingOk && retained){
            wait_retired(argv[5],retained);
            /* Loader CPU dispatch owners must be released while private ICD
             * tokens still exist; trusted host retirement has ended GPU access. */
            if(device && destroy_device){destroy_device(device,NULL);device=NULL;destroy_device=NULL;}
            if(instance && destroy_instance){destroy_instance(instance,NULL);instance=NULL;destroy_instance=NULL;}
            while(abandon(retained)!=RingOk)Sleep(10);
        }
        else while(status!=RingOk){Sleep(10);status=stop();}
    }
    /* After ordinary Ack or trusted host reap, backend accesses have ended.
     * Loader retains its own ICD reference while these CPU dispatch tables free. */
    if(device && destroy_device){destroy_device(device,NULL);device=NULL;}
    if(instance && destroy_instance){destroy_instance(instance,NULL);instance=NULL;}
    if(loader_module){while(!FreeLibrary(loader_module))Sleep(10);loader_module=NULL;}
    if(bootstrap_module){while(!FreeLibrary(bootstrap_module))Sleep(10);bootstrap_module=NULL;}
    if(system_dxgi_module){while(!FreeLibrary(system_dxgi_module))Sleep(10);system_dxgi_module=NULL;}
    if(GetModuleHandleA(argv[1]) || GetModuleHandleA(argv[2]) || GetModuleHandleA("waddle_vulkan_experimental.dll")) {
        fputs("Native fixture retained a released bootstrap/loader/ICD module\n",stderr);result=1;
    }
    if(!result)puts("Actual native GPU outputs and matching retirement Ack PASS; independent host result still required");
    return result;
}
