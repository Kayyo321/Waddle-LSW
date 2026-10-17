/** @file opencl_integration.c
 * @brief Native OpenCL exact-result gate through the authenticated guest ICD.
 * SDK entry-point names are external ABI identifiers. Every OpenCL owner is
 * released before the DLL; host retirement is independently proved by a
 * supervisor. Cross-compilation alone supplies no acceptance evidence.
 */
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "waddle/venus_tcp.h"

/** @brief External SDK functions borrowed from the loaded OpenCL module. */
#define ApiEntries(M) \
    M(get_platform_ids, clGetPlatformIDs) \
    M(get_device_ids, clGetDeviceIDs) \
    M(get_device_info, clGetDeviceInfo) \
    M(create_context, clCreateContext) \
    M(release_context, clReleaseContext) \
    M(create_queue, clCreateCommandQueue) \
    M(release_queue, clReleaseCommandQueue) \
    M(create_program, clCreateProgramWithSource) \
    M(build_program, clBuildProgram) \
    M(build_info, clGetProgramBuildInfo) \
    M(release_program, clReleaseProgram) \
    M(create_kernel, clCreateKernel) \
    M(kernel_info, clGetKernelWorkGroupInfo) \
    M(set_arg, clSetKernelArg) \
    M(release_kernel, clReleaseKernel) \
    M(create_buffer, clCreateBuffer) \
    M(release_buffer, clReleaseMemObject) \
    M(write_buffer, clEnqueueWriteBuffer) \
    M(read_buffer, clEnqueueReadBuffer) \
    M(copy_buffer, clEnqueueCopyBuffer) \
    M(map_buffer, clEnqueueMapBuffer) \
    M(unmap_buffer, clEnqueueUnmapMemObject) \
    M(enqueue_kernel, clEnqueueNDRangeKernel) \
    M(wait_events, clWaitForEvents) \
    M(event_info, clGetEventInfo) \
    M(release_event, clReleaseEvent) \
    M(finish, clFinish)

/** @brief Caller-owned immutable dispatch; module outlives all uses. */
typedef struct api_t {
#define DeclareApi(name, symbol) __typeof__(&symbol) name;
    ApiEntries(DeclareApi)
#undef DeclareApi
} api_t;

/** @brief Bootstrap ABI; caller owns DLL, start borrows config until return. */
typedef venus_ring_status_t (*bootstrap_start_t)(const char *, size_t);
/** @brief Quiescent stop; RingOk proves normal session stop. Main thread only. */
typedef venus_ring_status_t (*bootstrap_stop_t)(void);

/** @brief Fixed upper bound for owned CPU oracle storage; not a GPU tolerance. */
static const size_t MaxWords = 65536;
/** @brief Three integer kernels with deterministic CPU references. */
static const char Kernels[] =
    "__kernel void add(__global uint *a,__global uint *b,__global uint *o){"
    "size_t i=get_global_id(0);o[i]=a[i]+b[i];}"
    "__kernel void reduce(__global uint *o,__local uint *s){"
    "size_t i=get_local_id(0);s[i]=(uint)i;barrier(CLK_LOCAL_MEM_FENCE);"
    "if(i==0){uint sum=0;for(size_t j=0;j<get_local_size(0);j++)sum+=s[j];o[0]=sum;}}"
    "__kernel void count(__global uint *o){atomic_inc((volatile __global uint *)o);}"
    "__kernel void largest(__global uint *o){size_t i=get_global_id(0);o[i]=(uint)i*3u+7u;}";

/** @brief One complete OpenCL context lifetime, no owner escapes.
 * @param[in] api Nonnull borrowed dispatch; device borrowed root device.
 * @param[in] group Exact advertised maximum work-group size, 1..MaxWords.
 * @return Zero only for exact kernel/buffer/event/error assertions and cleanup.
 * @note Serial caller; compiler/driver may create threads. CPU arrays remain
 * live until queue finish on every path. Mapped memory is borrowed until unmap.
 */
static int run_lifetime(const api_t *api, cl_device_id device, size_t group)
{
    int result = 1;
    cl_int status = CL_SUCCESS;
    cl_context context = NULL;
    cl_command_queue queue = NULL;
    cl_program program = NULL, invalid = NULL;
    cl_kernel kernels[4] = {NULL};
    cl_mem buffers[3] = {NULL};
    cl_event events[4] = {NULL};
    void *mapped = NULL;
    cl_uint *words = calloc(MaxWords * 3, sizeof *words);
    if (!words) goto cleanup;
    cl_uint *left = words, *right = words + MaxWords, *output = words + MaxWords * 2;
    const size_t bytes = group * sizeof *words;
#define Require(expression) do { \
    status = (expression); \
    if (status != CL_SUCCESS) { \
        fprintf(stderr, "OpenCL line %d status %d\n", __LINE__, (int)status); \
        goto cleanup; \
    } \
} while (0)
    context = api->create_context(NULL, 1, &device, NULL, NULL, &status);
    if (!context || status != CL_SUCCESS) goto cleanup;
    queue = api->create_queue(context, device, 0, &status);
    if (!queue || status != CL_SUCCESS) goto cleanup;
    const char *source = Kernels;
    program = api->create_program(context, 1, &source, NULL, &status);
    if (!program || status != CL_SUCCESS) goto cleanup;
    Require(api->build_program(program, 1, &device, "", NULL, NULL));
    const char *names[] = {"add", "reduce", "count", "largest"};
    for (size_t i = 0; i < 4; ++i) {
        kernels[i] = api->create_kernel(program, names[i], &status);
        if (!kernels[i] || status != CL_SUCCESS) goto cleanup;
    }
    size_t kernel_group = 0;
    Require(api->kernel_info(kernels[3], device, CL_KERNEL_WORK_GROUP_SIZE,
                            sizeof kernel_group, &kernel_group, NULL));
    if (kernel_group < group) goto cleanup;
    for (size_t i = 0; i < 3; ++i) {
        buffers[i] = api->create_buffer(context, CL_MEM_READ_WRITE, bytes, NULL, &status);
        if (!buffers[i] || status != CL_SUCCESS) goto cleanup;
    }
    for (size_t i = 0; i < group; ++i) { left[i] = (cl_uint)i; right[i] = 9; }
    Require(api->write_buffer(queue, buffers[0], CL_FALSE, 0, bytes, left, 0, NULL, &events[0]));
    Require(api->write_buffer(queue, buffers[1], CL_FALSE, 0, bytes, right, 0, NULL, &events[1]));
    for (cl_uint i = 0; i < 3; ++i)
        Require(api->set_arg(kernels[0], i, sizeof buffers[i], &buffers[i]));
    Require(api->enqueue_kernel(queue, kernels[0], 1, NULL, &group, &group, 2, events, &events[2]));
    Require(api->read_buffer(queue, buffers[2], CL_FALSE, 0, bytes, output, 1, &events[2], &events[3]));
    Require(api->wait_events(1, &events[3]));
    for (size_t i = 0; i < group; ++i) if (output[i] != left[i] + right[i]) goto cleanup;
    for (size_t i = 0; i < 4; ++i) {
        cl_int execution = 1;
        Require(api->event_info(events[i], CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof execution, &execution, NULL));
        if (execution != CL_COMPLETE) goto cleanup;
    }
    Require(api->copy_buffer(queue, buffers[2], buffers[0], 0, 0, bytes, 0, NULL, NULL));
    mapped = api->map_buffer(queue, buffers[0], CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                            0, bytes, 0, NULL, NULL, &status);
    if (!mapped || status != CL_SUCCESS) goto cleanup;
    for (size_t i = 0; i < group; ++i) if (((cl_uint *)mapped)[i] != (cl_uint)i + 9) goto cleanup;
    memset(mapped, 0, bytes);
    Require(api->unmap_buffer(queue, buffers[0], mapped, 0, NULL, NULL));
    mapped = NULL;
    Require(api->set_arg(kernels[1], 0, sizeof buffers[2], &buffers[2]));
    Require(api->set_arg(kernels[1], 1, bytes, NULL));
    Require(api->enqueue_kernel(queue, kernels[1], 1, NULL, &group, &group, 0, NULL, NULL));
    Require(api->read_buffer(queue, buffers[2], CL_TRUE, 0, sizeof *output, output, 0, NULL, NULL));
    if (output[0] != (cl_uint)(group * (group - 1) / 2)) goto cleanup;
    Require(api->set_arg(kernels[2], 0, sizeof buffers[0], &buffers[0]));
    Require(api->enqueue_kernel(queue, kernels[2], 1, NULL, &group, &group, 0, NULL, NULL));
    Require(api->read_buffer(queue, buffers[0], CL_TRUE, 0, sizeof *output, output, 0, NULL, NULL));
    if (output[0] != group) goto cleanup;
    Require(api->set_arg(kernels[3], 0, sizeof buffers[2], &buffers[2]));
    Require(api->enqueue_kernel(queue, kernels[3], 1, NULL, &group, &group, 0, NULL, NULL));
    Require(api->read_buffer(queue, buffers[2], CL_TRUE, 0, bytes, output, 0, NULL, NULL));
    for (size_t i = 0; i < group; ++i) if (output[i] != (cl_uint)i * 3 + 7) goto cleanup;
    /* Invalid ranges and argument sizes must not turn into GPU memory access. */
    if (api->read_buffer(queue, buffers[0], CL_TRUE, bytes, 1, output, 0, NULL, NULL) != CL_INVALID_VALUE) goto cleanup;
    if (api->set_arg(kernels[3], 0, 1, &buffers[2]) != CL_INVALID_ARG_SIZE) goto cleanup;
    source = "__kernel void deliberately_invalid( {";
    invalid = api->create_program(context, 1, &source, NULL, &status);
    if (!invalid || status != CL_SUCCESS) goto cleanup;
    if (api->build_program(invalid, 1, &device, "", NULL, NULL) != CL_BUILD_PROGRAM_FAILURE) goto cleanup;
    size_t log_bytes = 0;
    Require(api->build_info(invalid, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_bytes));
    if (log_bytes < 2 || log_bytes > MaxWords * sizeof *output) goto cleanup;
    Require(api->build_info(invalid, device, CL_PROGRAM_BUILD_LOG, log_bytes, output, NULL));
    if (!((char *)output)[0] || ((char *)output)[log_bytes - 1]) goto cleanup;
    result = 0;
cleanup:
    if (mapped && api->unmap_buffer(queue, buffers[0], mapped, 0, NULL, NULL) != CL_SUCCESS) result = 1;
    if (queue && api->finish(queue) != CL_SUCCESS) result = 1;
    for (size_t i = 0; i < 4; ++i) {
        if (events[i] && api->release_event(events[i]) != CL_SUCCESS) result = 1;
        if (kernels[i] && api->release_kernel(kernels[i]) != CL_SUCCESS) result = 1;
    }
    for (size_t i = 0; i < 3; ++i)
        if (buffers[i] && api->release_buffer(buffers[i]) != CL_SUCCESS) result = 1;
    if (invalid && api->release_program(invalid) != CL_SUCCESS) result = 1;
    if (program && api->release_program(program) != CL_SUCCESS) result = 1;
    if (queue && api->release_queue(queue) != CL_SUCCESS) result = 1;
    if (context && api->release_context(context) != CL_SUCCESS) result = 1;
    free(words);
    words = NULL;
    if (!result) puts("OpenCL exact add/local-barrier/atomics/max-group/buffers/events/build-error lifetime passed.");
    fflush(stdout);
    return result;
#undef Require
}

/** @brief Execute one module graph; all paths borrowed from the supervisor.
 * @param[in] argc/argv Exactly six: executable, loader, OpenCL.dll, manifest,
 * bootstrap DLL, private config; every file must use an absolute drive path.
 * @return Zero after eight full contexts and natural bootstrap stop, otherwise
 * failure. A failed stop retains modules/process for external owner resolution.
 * @note Main thread only. Unloads OpenCL before loader/bootstrap. Native heaps
 * require independent capture; successful process exit does not prove no leak.
 */
int wmain(int argc, wchar_t **argv)
{
    if (argc != 6) return 2;
    for (int i = 1; i < argc; ++i)
        if (wcslen(argv[i]) < 3 || argv[i][1] != L':' || argv[i][2] != L'\\' ||
            GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) return 2;
    if (!SetEnvironmentVariableW(L"VK_DRIVER_FILES", argv[3]) ||
        !SetEnvironmentVariableW(L"VK_ICD_FILENAMES", NULL) ||
        !SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", NULL) ||
        !SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~all~")) return 2;
    int result = 1;
    HMODULE bootstrap = NULL, loader = NULL, opencl = NULL;
    bootstrap_start_t start = NULL;
    bootstrap_stop_t stop = NULL;
    bootstrap = LoadLibraryExW(argv[4], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bootstrap) goto cleanup;
    FARPROC procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_start");
    _Static_assert(sizeof start == sizeof procedure, "Windows function pointer ABI");
    memcpy(&start, &procedure, sizeof start);
    procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_stop");
    memcpy(&stop, &procedure, sizeof stop);
    if (!start || !stop) goto cleanup;
    char config[32768];
    int config_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[5], -1,
                                          config, sizeof config, NULL, NULL);
    if (!config_bytes || start(config, (size_t)config_bytes - 1) != RingOk) goto cleanup;
    loader = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) goto cleanup;
    opencl = LoadLibraryExW(argv[2], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!opencl) goto cleanup;
    api_t api = {0};
#define LoadApi(name, symbol) do { \
    procedure = GetProcAddress(opencl, #symbol); \
    if (!procedure) goto cleanup; \
    _Static_assert(sizeof api.name == sizeof procedure, "Windows function pointer ABI"); \
    memcpy(&api.name, &procedure, sizeof procedure); \
} while (0);
    ApiEntries(LoadApi)
#undef LoadApi
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_uint count = 0;
    cl_int status = api.get_platform_ids(1, &platform, &count);
    printf("OpenCL platform discovery status=%d count=%u\n", (int)status, (unsigned)count); fflush(stdout);
    if (status != CL_SUCCESS || count != 1 || !platform) goto cleanup;
    if (api.get_device_ids(platform, CL_DEVICE_TYPE_ALL, 1, &device, &count) != CL_SUCCESS || count != 1) goto cleanup;
    char name[512] = {0};
    cl_device_type type = 0;
    cl_uint vendor = 0;
    size_t group = 0;
    cl_ulong local_bytes = 0;
    if (api.get_device_info(device, CL_DEVICE_NAME, sizeof name, name, NULL) != CL_SUCCESS ||
        api.get_device_info(device, CL_DEVICE_TYPE, sizeof type, &type, NULL) != CL_SUCCESS ||
        api.get_device_info(device, CL_DEVICE_VENDOR_ID, sizeof vendor, &vendor, NULL) != CL_SUCCESS ||
        api.get_device_info(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof group, &group, NULL) != CL_SUCCESS ||
        api.get_device_info(device, CL_DEVICE_LOCAL_MEM_SIZE, sizeof local_bytes, &local_bytes, NULL) != CL_SUCCESS) goto cleanup;
    if (!memchr(name, 0, sizeof name) || !strstr(name, "RTX 5080") || vendor != 0x10de ||
        !(type & CL_DEVICE_TYPE_GPU) || (type & CL_DEVICE_TYPE_CPU) || !group || group > MaxWords ||
        local_bytes < group * sizeof(cl_uint)) goto cleanup;
    printf("OpenCL selected GPU=%s vendor=%x maximum_group=%llu\n", name, vendor, (unsigned long long)group); fflush(stdout);
    for (unsigned cycle = 0; cycle < 8; ++cycle)
        if (run_lifetime(&api, device, group)) goto cleanup;
    result = 0;
cleanup:
    if (result) fprintf(stderr, "OpenCL acceptance failed Win32=%lu\n", (unsigned long)GetLastError());
    if (opencl && !FreeLibrary(opencl)) result = 1;
    if (stop && stop() != RingOk) {
        fputs("OpenCL retained bootstrap: supervisor must prove retirement; modules preserved.\n", stderr);
        fflush(stderr);
        for (;;) Sleep(1000);
    }
    if (loader && !FreeLibrary(loader)) result = 1;
    if (bootstrap && !FreeLibrary(bootstrap)) result = 1;
    if (GetModuleHandleW(argv[1]) || GetModuleHandleW(argv[2]) || GetModuleHandleW(argv[4]) ||
        GetModuleHandleW(L"waddle_vulkan_experimental.dll")) result = 1;
    if (!result) puts("OpenCL eight exact context lifetimes and native module retirement passed.");
    return result;
}
