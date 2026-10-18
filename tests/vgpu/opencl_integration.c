/** @file opencl_integration.c
 * @brief Native OpenCL exact-result gate through the authenticated guest ICD.
 * SDK entry-point names are external ABI identifiers. Every OpenCL owner is
 * released before the DLL; host retirement is independently proved by a
 * supervisor. Cross-compilation alone supplies no acceptance evidence.
 */
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "waddle/venus_tcp.h"
#include "waddle/clvk.h"
#include "loader_environment_windows.h"

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
    M(create_sub_buffer, clCreateSubBuffer) \
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
/** @brief Quiescent stop; RingOk proves normal session stop. Serial caller. */
typedef venus_ring_status_t (*bootstrap_stop_t)(void);
/** @brief Borrowed copied session identity, no ownership transfer; main only. */
typedef uint64_t (*bootstrap_session_t)(void);
/** @brief Release pending session only after exact external retirement proof. */
typedef venus_ring_status_t (*bootstrap_abandon_t)(uint64_t);

/** @brief Wait for the supervisor's exact session retirement record.
 * @param[in] path Trusted fresh absolute UTF-16 receipt path, borrowed.
 * @param[in] identity Nonzero retained session identity.
 * @note Failure retains ownership until the supervisor proves host retirement.
 * Sole caller thread; each transient receipt handle is closed before retry.
 */
static void wait_retired(const wchar_t *path,uint64_t identity) {
    char expected[64];int length=snprintf(expected,sizeof expected,"retired_session=%016llx\n",(unsigned long long)identity);
    if(length<=0 || (size_t)length>=sizeof expected)abort();
    fprintf(stderr,"Retained native session=%016llx awaiting trusted actual worker retirement\n",(unsigned long long)identity);fflush(stderr);
    for(;;) {
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information={0};char actual[65];DWORD count=0;int match=0;
            if(GetFileType(file)==FILE_TYPE_DISK && GetFileInformationByHandle(file,&information) &&
                !(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && information.nNumberOfLinks==1 &&
                !information.nFileSizeHigh && information.nFileSizeLow==(DWORD)length &&
                ReadFile(file,actual,sizeof actual,&count,NULL) && count==(DWORD)length && !memcmp(actual,expected,(size_t)length))match=1;
            while(!CloseHandle(file))Sleep(10);
            if(match)return;
        }
        Sleep(10);
    }
}


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
 * @param[in] shutdown Optional private dispatch; null for concurrent workers.
 * Busy probes run only with exclusive CL caller ownership. The DLL outlives
 * every call and every context and nested caller thread.
 * @return Zero only for exact kernel/buffer/event/error assertions and cleanup.
 * @note Serial caller; compiler/driver may create threads. CPU arrays remain
 * live until queue finish on every path. Mapped memory is borrowed until unmap.
 */
static int run_lifetime(const api_t *api, cl_device_id device, size_t group,
                        clvk_shutdown_t shutdown)
{
    int result = 1;
    cl_int status = CL_SUCCESS;
    cl_context context = NULL;
    cl_context foreign_context = NULL;
    cl_command_queue queue = NULL;
    cl_command_queue foreign_queue = NULL;
    cl_program program = NULL, invalid = NULL;
    cl_kernel kernels[4] = {NULL};
    cl_mem buffers[3] = {NULL};
    cl_mem foreign_buffer = NULL, alias_root = NULL, alias_source = NULL, alias_destination = NULL;
    cl_event events[4] = {NULL};
    cl_event foreign_event = NULL, rejected_event = NULL;
    void *mapped = NULL;
    void *probe_mapped = NULL;
    cl_command_queue probe_map_queue = NULL;
    cl_mem probe_map_buffer = NULL;
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
#define RequireOracleSuccess(label, expression) do { \
    status = (expression); \
    if (status != CL_SUCCESS) { \
        fprintf(stderr, "OpenCL buffer oracle %s expected status %d, got %d\n", \
                label, (int)CL_SUCCESS, (int)status); \
        goto cleanup; \
    } \
} while (0)
#define RequireOracleError(label, expected, expression) do { \
    rejected_event = NULL; \
    status = (expression); \
    if (status != (expected) || rejected_event != NULL) { \
        fprintf(stderr, "OpenCL buffer oracle %s expected status %d and null event, got status %d event %p\n", \
                label, (int)(expected), (int)status, (void *)rejected_event); \
        if (rejected_event) (void)api->release_event(rejected_event); \
        rejected_event = NULL; \
        goto cleanup; \
    } \
    rejected_event = NULL; \
} while (0)
#define RequireMapError(label, expected, map_queue, map_object, map_offset, map_size, wait_count, wait_list) do { \
    cl_int map_status = CL_SUCCESS; \
    rejected_event = NULL; \
    probe_mapped = api->map_buffer(map_queue, map_object, CL_TRUE, CL_MAP_READ, \
        map_offset, map_size, wait_count, wait_list, &rejected_event, &map_status); \
    if (probe_mapped || map_status != (expected) || rejected_event != NULL) { \
        fprintf(stderr, "OpenCL buffer map oracle %s expected status %d, null mapping and null event, got status %d pointer %p event %p\n", \
                label, (int)(expected), (int)map_status, probe_mapped, (void *)rejected_event); \
        if (probe_mapped) { probe_map_queue = map_queue; probe_map_buffer = map_object; } \
        if (rejected_event) (void)api->release_event(rejected_event); \
        rejected_event = NULL; \
        goto cleanup; \
    } \
    rejected_event = NULL; \
} while (0)
    context = api->create_context(NULL, 1, &device, NULL, NULL, &status);
    if (!context || status != CL_SUCCESS) goto cleanup;
    if (shutdown && (shutdown(ClvkShutdownAbi + 1) != ClvkShutdownVersion ||
        shutdown(ClvkShutdownAbi) != ClvkShutdownBusy)) goto cleanup;
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
    /* Exercise accepted exact-end spans before rejecting malformed inputs. */
    const size_t end_offset = bytes - sizeof(cl_uint);
    const cl_uint end_value = left[group - 1];
    RequireOracleSuccess("exact-end write", api->write_buffer(queue, buffers[0], CL_TRUE,
        end_offset, sizeof end_value, &end_value, 0, NULL, NULL));
    cl_uint end_readback = 0;
    RequireOracleSuccess("exact-end read", api->read_buffer(queue, buffers[0], CL_TRUE,
        end_offset, sizeof end_readback, &end_readback, 0, NULL, NULL));
    if (end_readback != end_value) {
        fprintf(stderr, "OpenCL buffer oracle exact-end readback mismatch: got %u expected %u\n",
                end_readback, end_value);
        goto cleanup;
    }
    RequireOracleSuccess("exact-end copy", api->copy_buffer(queue, buffers[0], buffers[1],
        end_offset, end_offset, sizeof end_value, 0, NULL, NULL));
    cl_int map_status = CL_SUCCESS;
    probe_mapped = api->map_buffer(queue, buffers[1], CL_TRUE, CL_MAP_READ,
        end_offset, sizeof end_value, 0, NULL, NULL, &map_status);
    if (probe_mapped) { probe_map_queue = queue; probe_map_buffer = buffers[1]; }
    if (!probe_mapped || map_status != CL_SUCCESS ||
        *(const cl_uint *)probe_mapped != end_value) {
        fprintf(stderr, "OpenCL buffer oracle exact-end map expected status %d/value %u, got status %d pointer %p\n",
                (int)CL_SUCCESS, end_value, (int)map_status, probe_mapped);
        goto cleanup;
    }
    RequireOracleSuccess("exact-end unmap", api->unmap_buffer(queue, buffers[1], probe_mapped,
        0, NULL, NULL));
    probe_mapped = NULL;

    /* Invalid ranges and arguments must reject before event creation or host access. */
    RequireOracleError("read zero length", CL_INVALID_VALUE,
        api->read_buffer(queue, buffers[0], CL_TRUE, 0, 0, output, 0, NULL, &rejected_event));
    RequireOracleError("write zero length", CL_INVALID_VALUE,
        api->write_buffer(queue, buffers[0], CL_TRUE, 0, 0, left, 0, NULL, &rejected_event));
    RequireOracleError("copy zero length", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], 0, 0, 0, 0, NULL, &rejected_event));
    RequireMapError("map zero length", CL_INVALID_VALUE, queue, buffers[0], 0, 0, 0, NULL);

    RequireOracleError("read offset past end", CL_INVALID_VALUE,
        api->read_buffer(queue, buffers[0], CL_TRUE, bytes + 1, 1, output, 0, NULL, &rejected_event));
    RequireOracleError("write offset past end", CL_INVALID_VALUE,
        api->write_buffer(queue, buffers[0], CL_TRUE, bytes + 1, 1, left, 0, NULL, &rejected_event));
    RequireOracleError("copy source offset past end", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], bytes + 1, 0, 1, 0, NULL, &rejected_event));
    RequireOracleError("copy destination offset past end", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], 0, bytes + 1, 1, 0, NULL, &rejected_event));
    RequireMapError("map offset past end", CL_INVALID_VALUE, queue, buffers[0], bytes + 1, 1, 0, NULL);

    RequireOracleError("read size past end", CL_INVALID_VALUE,
        api->read_buffer(queue, buffers[0], CL_TRUE, bytes - 1, 2, output, 0, NULL, &rejected_event));
    RequireOracleError("write size past end", CL_INVALID_VALUE,
        api->write_buffer(queue, buffers[0], CL_TRUE, bytes - 1, 2, left, 0, NULL, &rejected_event));
    RequireOracleError("copy source size past end", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], bytes - 1, 0, 2, 0, NULL, &rejected_event));
    RequireOracleError("copy destination size past end", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], 0, bytes - 1, 2, 0, NULL, &rejected_event));
    RequireMapError("map size past end", CL_INVALID_VALUE, queue, buffers[0], bytes - 1, 2, 0, NULL);

    RequireOracleError("read SIZE_MAX wrap", CL_INVALID_VALUE,
        api->read_buffer(queue, buffers[0], CL_TRUE, SIZE_MAX - 3, 8, output, 0, NULL, &rejected_event));
    RequireOracleError("write SIZE_MAX wrap", CL_INVALID_VALUE,
        api->write_buffer(queue, buffers[0], CL_TRUE, SIZE_MAX - 3, 8, left, 0, NULL, &rejected_event));
    RequireOracleError("copy source SIZE_MAX wrap", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], SIZE_MAX - 3, 0, 8, 0, NULL, &rejected_event));
    RequireOracleError("copy destination SIZE_MAX wrap", CL_INVALID_VALUE,
        api->copy_buffer(queue, buffers[0], buffers[1], 0, SIZE_MAX - 3, 8, 0, NULL, &rejected_event));
    RequireMapError("map SIZE_MAX wrap", CL_INVALID_VALUE, queue, buffers[0], SIZE_MAX - 3, 8, 0, NULL);

    RequireOracleError("read null host pointer", CL_INVALID_VALUE,
        api->read_buffer(queue, buffers[0], CL_TRUE, 0, sizeof *output, NULL, 0, NULL, &rejected_event));
    RequireOracleError("write null host pointer", CL_INVALID_VALUE,
        api->write_buffer(queue, buffers[0], CL_TRUE, 0, sizeof *left, NULL, 0, NULL, &rejected_event));
    RequireOracleError("same-buffer overlap", CL_MEM_COPY_OVERLAP,
        api->copy_buffer(queue, buffers[0], buffers[0], 0, 1, 2, 0, NULL, &rejected_event));

    /* The serial lifetimes own a real second context for context/event mismatch oracles. */
    if (shutdown) {
        cl_uint alignment_bits = 0;
        Require(api->get_device_info(device, CL_DEVICE_MEM_BASE_ADDR_ALIGN,
            sizeof alignment_bits, &alignment_bits, NULL));
        cl_uint divisor_left = alignment_bits, divisor_right = 8;
        while (divisor_right) {
            cl_uint remainder = divisor_left % divisor_right;
            divisor_left = divisor_right;
            divisor_right = remainder;
        }
        if (!alignment_bits || !divisor_left) {
            fprintf(stderr, "OpenCL alias oracle requires nonzero base alignment, got %u bits\n",
                    alignment_bits);
            goto cleanup;
        }
        const size_t alignment_bytes = alignment_bits / divisor_left;
        if (!alignment_bytes || alignment_bytes > MaxWords * sizeof(cl_uint) / 4) {
            fprintf(stderr, "OpenCL alias oracle alignment out of bound: %llu bytes\n",
                    (unsigned long long)alignment_bytes);
            goto cleanup;
        }
        const size_t alias_root_bytes = alignment_bytes * 4;
        const cl_buffer_region source_region = {0, alignment_bytes * 3};
        const cl_buffer_region destination_region = {alignment_bytes, alignment_bytes * 3};
        alias_root = api->create_buffer(context, CL_MEM_READ_WRITE, alias_root_bytes, NULL, &status);
        if (!alias_root || status != CL_SUCCESS) goto cleanup;
        alias_source = api->create_sub_buffer(alias_root, CL_MEM_READ_WRITE,
            CL_BUFFER_CREATE_TYPE_REGION, &source_region, &status);
        if (!alias_source || status != CL_SUCCESS) goto cleanup;
        alias_destination = api->create_sub_buffer(alias_root, CL_MEM_READ_WRITE,
            CL_BUFFER_CREATE_TYPE_REGION, &destination_region, &status);
        if (!alias_destination || status != CL_SUCCESS) goto cleanup;
        RequireOracleError("overlapping sibling sub-buffers", CL_MEM_COPY_OVERLAP,
            api->copy_buffer(queue, alias_source, alias_destination, 0, 0,
                alignment_bytes * 2, 0, NULL, &rejected_event));

        foreign_context = api->create_context(NULL, 1, &device, NULL, NULL, &status);
        if (!foreign_context || status != CL_SUCCESS) goto cleanup;
        foreign_queue = api->create_queue(foreign_context, device, 0, &status);
        if (!foreign_queue || status != CL_SUCCESS) goto cleanup;
        foreign_buffer = api->create_buffer(foreign_context, CL_MEM_READ_WRITE, bytes, NULL, &status);
        if (!foreign_buffer || status != CL_SUCCESS) goto cleanup;
        cl_uint foreign_word = 0x12345678u;
        Require(api->write_buffer(foreign_queue, foreign_buffer, CL_TRUE, 0,
            sizeof foreign_word, &foreign_word, 0, NULL, &foreign_event));
        if (!foreign_event) goto cleanup;
        RequireOracleError("read foreign-context buffer", CL_INVALID_CONTEXT,
            api->read_buffer(queue, foreign_buffer, CL_TRUE, 0, sizeof *output,
                output, 0, NULL, &rejected_event));
        RequireOracleError("write foreign-context buffer", CL_INVALID_CONTEXT,
            api->write_buffer(queue, foreign_buffer, CL_TRUE, 0, sizeof *left,
                left, 0, NULL, &rejected_event));
        RequireOracleError("copy foreign-context source", CL_INVALID_CONTEXT,
            api->copy_buffer(queue, foreign_buffer, buffers[0], 0, 0,
                sizeof *output, 0, NULL, &rejected_event));
        RequireOracleError("copy foreign-context destination", CL_INVALID_CONTEXT,
            api->copy_buffer(queue, buffers[0], foreign_buffer, 0, 0,
                sizeof *output, 0, NULL, &rejected_event));
        RequireMapError("map foreign-context buffer", CL_INVALID_CONTEXT,
            queue, foreign_buffer, 0, sizeof *output, 0, NULL);
        RequireOracleError("read foreign-context wait event", CL_INVALID_CONTEXT,
            api->read_buffer(queue, buffers[0], CL_TRUE, 0, sizeof *output,
                output, 1, &foreign_event, &rejected_event));
        RequireOracleError("write foreign-context wait event", CL_INVALID_CONTEXT,
            api->write_buffer(queue, buffers[0], CL_TRUE, 0, sizeof *left,
                left, 1, &foreign_event, &rejected_event));
        RequireOracleError("copy foreign-context wait event", CL_INVALID_CONTEXT,
            api->copy_buffer(queue, buffers[0], buffers[1], 0, 0,
                sizeof *output, 1, &foreign_event, &rejected_event));
        RequireMapError("map foreign-context wait event", CL_INVALID_CONTEXT,
            queue, buffers[0], 0, sizeof *output, 1, &foreign_event);
    }
    puts("OpenCL malformed buffer range, pointer, overlap, context and event oracles passed.");
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
    if (probe_mapped && probe_map_queue && probe_map_buffer &&
        api->unmap_buffer(probe_map_queue, probe_map_buffer, probe_mapped, 0, NULL, NULL) != CL_SUCCESS) result = 1;
    if (queue && api->finish(queue) != CL_SUCCESS) result = 1;
    if (foreign_queue && api->finish(foreign_queue) != CL_SUCCESS) result = 1;
    if (foreign_event && api->release_event(foreign_event) != CL_SUCCESS) result = 1;
    if (foreign_buffer && api->release_buffer(foreign_buffer) != CL_SUCCESS) result = 1;
    if (foreign_queue && api->release_queue(foreign_queue) != CL_SUCCESS) result = 1;
    if (foreign_context && api->release_context(foreign_context) != CL_SUCCESS) result = 1;
    if (alias_source && api->release_buffer(alias_source) != CL_SUCCESS) result = 1;
    if (alias_destination && api->release_buffer(alias_destination) != CL_SUCCESS) result = 1;
    if (alias_root && api->release_buffer(alias_root) != CL_SUCCESS) result = 1;
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
#undef RequireOracleSuccess
#undef RequireOracleError
#undef RequireMapError
}

/** @brief Parent-owned concurrent context runner; all borrows outlive join. */
typedef struct context_call_t {
    const api_t *api;
    cl_device_id device;
    size_t group;
} context_call_t;

/** @brief Run three disjoint context/queue/buffer graphs on the shared device.
 * @param[in] opaque Nonnull parent-owned context_call_t, borrowed until return.
 * @return Zero only when all exact lifetime oracles and cleanup pass.
 * @note Shutdown is excluded until both application callers join; no callback
 * or context escapes. DLLs remain mapped through this CRT thread's TLS exit.
 */
static unsigned __stdcall run_contexts(void *opaque) {
    const context_call_t *call = opaque;
    for (unsigned cycle = 0; cycle < 3; ++cycle)
        if (run_lifetime(call->api, call->device, call->group, NULL)) return 1;
    return 0;
}

/** @brief Joined caller-thread state; modules transfer to the joining parent.
 * argc/argv are borrowed until join; only the worker writes module references.
 */
typedef struct native_work_t {
    int argc;
    wchar_t **argv;
    HMODULE bootstrap;
    HMODULE loader;
    HMODULE opencl;
} native_work_t;

/** @brief Execute one module graph; all paths borrowed from the supervisor.
 * @param[in] argc/argv Exactly seven: executable, loader, OpenCL.dll, manifest,
 * bootstrap DLL, private config, fresh retirement receipt. Existing inputs use absolute drive paths.
 * @return Zero after eight full contexts and natural bootstrap stop, otherwise
 * failure. A failed stop retains modules/process for external owner resolution.
 * @note Worker thread only. Parent keeps DLLs mapped through worker TLS/FLS
 * destruction, then unloads OpenCL before loader/bootstrap. Native heaps require
 * independent capture; successful process exit does not prove no leak.
 */
static unsigned __stdcall run_native_work(void *opaque)
{
    native_work_t *work = opaque;
    int argc = work->argc;
    wchar_t **argv = work->argv;
    if (argc != 7) return 2;
    for (int i = 1; i < 6; ++i)
        if (wcslen(argv[i]) < 3 || argv[i][1] != L':' || argv[i][2] != L'\\' ||
            GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) return 2;
    if (wcslen(argv[6]) < 3 || argv[6][1] != L':' || argv[6][2] != L'\\' ||
        GetFileAttributesW(argv[6]) != INVALID_FILE_ATTRIBUTES) return 2;
    int result = 1;
    HMODULE bootstrap = NULL, loader = NULL, opencl = NULL;
    api_t api = {0};
    cl_uint count = 0;
    bootstrap_start_t start = NULL;
    bootstrap_stop_t stop = NULL;
    bootstrap_session_t session = NULL;
    bootstrap_abandon_t abandon = NULL;
    clvk_shutdown_t shutdown = NULL;
    bootstrap = LoadLibraryExW(argv[4], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bootstrap) goto cleanup;
    FARPROC procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_start");
    _Static_assert(sizeof start == sizeof procedure, "Windows function pointer ABI");
    memcpy(&start, &procedure, sizeof start);
    procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_stop");
    memcpy(&stop, &procedure, sizeof stop);
    procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_session");
    memcpy(&session, &procedure, sizeof session);
    procedure = GetProcAddress(bootstrap, "venus_tcp_bootstrap_abandon");
    memcpy(&abandon, &procedure, sizeof abandon);
    if (!start || !stop || !session || !abandon) goto cleanup;
    char config[VenusTcpMaxWindowsPathBytes];
    int config_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[5], -1,
                                          config, sizeof config, NULL, NULL);
    if (!config_bytes) goto cleanup;
    ULONGLONG load_started = GetTickCount64();
    loader = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) goto cleanup;
    opencl = LoadLibraryExW(argv[2], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!opencl) goto cleanup;
    procedure = GetProcAddress(opencl, "waddle_clvk_shutdown");
    _Static_assert(sizeof shutdown == sizeof procedure, "Windows private C ABI");
    memcpy(&shutdown, &procedure, sizeof shutdown);
    if (!shutdown || shutdown(ClvkShutdownAbi + 1) != ClvkShutdownVersion) goto cleanup;
#define LoadApi(name, symbol) do { \
    procedure = GetProcAddress(opencl, #symbol); \
    if (!procedure) goto cleanup; \
    _Static_assert(sizeof api.name == sizeof procedure, "Windows function pointer ABI"); \
    memcpy(&api.name, &procedure, sizeof procedure); \
} while (0);
    ApiEntries(LoadApi)
#undef LoadApi
    printf("OpenCL modules and dispatch ready after %llu ms\n",
           (unsigned long long)(GetTickCount64() - load_started)); fflush(stdout);
    /* clvk initializes Vulkan lazily at platform discovery. Load and resolve
     * its compiler DLL before opening the bounded, authenticated transport so
     * DLL initialization cannot consume the server's idle request deadline. */
    if (start(config, (size_t)config_bytes) != RingOk) goto cleanup;
    printf("Authenticated real OpenCL session=%016llx\n", (unsigned long long)session()); fflush(stdout);
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_int status = api.get_platform_ids(1, &platform, &count);
    printf("OpenCL platform discovery status=%d count=%u\n", (int)status, (unsigned)count); fflush(stdout);
    if (status != CL_SUCCESS || count != 1 || !platform) goto cleanup;
    /* A real host may expose several adapters. Select exactly one matching GPU
     * without accepting a CPU renderer or depending on enumeration order. */
    cl_device_id devices[16] = {NULL};
    count = 0;
    if (api.get_device_ids(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &count) != CL_SUCCESS ||
        !count || count > sizeof devices / sizeof devices[0]) goto cleanup;
    cl_uint device_count = count;
    if (api.get_device_ids(platform, CL_DEVICE_TYPE_ALL, device_count, devices, &count) != CL_SUCCESS ||
        count != device_count) goto cleanup;
    char name[512] = {0};
    cl_device_type type = 0;
    cl_uint vendor = 0;
    cl_uint matches = 0;
    for (cl_uint i = 0; i < device_count; ++i) {
        memset(name, 0, sizeof name);
        if (!devices[i] ||
            api.get_device_info(devices[i], CL_DEVICE_NAME, sizeof name, name, NULL) != CL_SUCCESS ||
            api.get_device_info(devices[i], CL_DEVICE_TYPE, sizeof type, &type, NULL) != CL_SUCCESS ||
            api.get_device_info(devices[i], CL_DEVICE_VENDOR_ID, sizeof vendor, &vendor, NULL) != CL_SUCCESS ||
            !memchr(name, 0, sizeof name)) goto cleanup;
        printf("OpenCL discovered adapter=%s vendor=%x type=%llu\n", name, vendor,
               (unsigned long long)type); fflush(stdout);
        if (strstr(name, "RTX 5080") && vendor == 0x10de &&
            (type & CL_DEVICE_TYPE_GPU) && !(type & CL_DEVICE_TYPE_CPU)) {
            device = devices[i];
            ++matches;
        }
    }
    if (matches != 1) goto cleanup;
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
    for (unsigned cycle = 0; cycle < 2; ++cycle)
        if (run_lifetime(&api, device, group, shutdown)) goto cleanup;
    context_call_t calls[2] = {{&api, device, group}, {&api, device, group}};
    HANDLE threads[2] = {NULL, NULL};
    int concurrent_failed = 0;
    for (size_t i = 0; i < 2; ++i) {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, run_contexts, &calls[i], 0, NULL);
        if (!threads[i]) { concurrent_failed = 1; break; }
    }
    for (size_t i = 0; i < 2; ++i) {
        if (!threads[i]) continue;
        DWORD code = 1;
        if (WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(threads[i], &code)) abort();
        if (!CloseHandle(threads[i])) abort();
        if (code != 0) concurrent_failed = 1;
    }
    if (concurrent_failed) goto cleanup;
    puts("OpenCL concurrent independent context callers joined with exact results."); fflush(stdout);
    result = 0;
cleanup:
    if (result) fprintf(stderr, "OpenCL acceptance failed Win32=%lu\n", (unsigned long)GetLastError());
    if (shutdown) {
        int32_t shutdown_status;
        ULONGLONG started = GetTickCount64();
        int reported = 0;
        /* Never unload code while a runtime owner remains. A stuck owner is a
         * failed supervised run; retain modules/process for diagnosis rather
         * than hiding it with forced destruction. No success deadline changes. */
        while ((shutdown_status = shutdown(ClvkShutdownAbi)) == ClvkShutdownBusy) {
            if (!reported && GetTickCount64() - started >= 5000) {
                fputs("Retained clvk owners prevent terminal shutdown\n", stderr);
                fflush(stderr); reported = 1; result = 1;
            }
            Sleep(1);
        }
        if (shutdown_status != ClvkShutdownOk) {
            fprintf(stderr, "Retained clvk state status=%d\n", (int)shutdown_status); fflush(stderr);
            for (;;) Sleep(10);
        }
        if (shutdown(ClvkShutdownAbi) != ClvkShutdownOk) result = 1;
        if (api.get_platform_ids && api.get_platform_ids(0, NULL, &count) != CL_INVALID_PLATFORM) result = 1;
    }
    if (stop && session && abandon && stop() != RingOk) {
        result = 1;
        uint64_t retained = session();
        if (retained) {
            wait_retired(argv[6], retained);
            while (abandon(retained) != RingOk) Sleep(10);
        }
    }
    work->opencl = opencl;
    work->loader = loader;
    work->bootstrap = bootstrap;
    return (unsigned)result;
}

/** @brief Retry exact process-environment restoration while retaining snapshots.
 * @param[in,out] guard Active owner returned by the loader environment guard.
 * @return One if any restoration attempt failed, zero otherwise; returns only
 * after every original value is restored. A retry cannot erase a failed cycle.
 * @note Sole parent thread, after every OpenCL/loader/bootstrap/ICD module is gone.
 */
static int restore_loader_environment(waddle_loader_environment_guard_t *guard)
{
    int failed = 0;
    waddle_loader_environment_status_t status = WaddleLoaderEnvironmentRestoreFailed;
    do {
        status = waddle_loader_environment_restore(guard);
        if (status != WaddleLoaderEnvironmentOk) {
            failed = 1;
            fprintf(stderr, "Retained OpenCL loader environment awaiting restoration mask=%lu error=%lu\n",
                    (unsigned long)guard->restore_failed_mask, (unsigned long)GetLastError());
            fflush(stderr);
            Sleep(10);
        }
    } while (status != WaddleLoaderEnvironmentOk);
    return failed;
}

/** @brief Keep vendor DLLs mapped until calling-thread TLS/FLS has retired.
 * @param[in] argc/argv Borrowed Windows command line, as documented above.
 * @return Zero only after exact kernels, terminal shutdown, natural host stop,
 * joined worker and complete module retirement. Main thread owns the thread
 * handle and transferred module references and closes/releases each once.
 * The parent-owned loader snapshot remains active until all vendor modules exit.
 */
static int run_opencl_cycle(int argc, wchar_t **argv) {
    if (argc != 7) return 2;
    for (int i = 1; i < 6; ++i)
        if (wcslen(argv[i]) < 3 || argv[i][1] != L':' || argv[i][2] != L'\\' ||
            GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) return 2;
    if (wcslen(argv[6]) < 3 || argv[6][1] != L':' || argv[6][2] != L'\\' ||
        GetFileAttributesW(argv[6]) != INVALID_FILE_ATTRIBUTES) return 2;

    waddle_loader_environment_guard_t environment;
    waddle_loader_environment_guard_init(&environment);
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, argv[3]}, {FALSE, NULL}, {FALSE, NULL}, {TRUE, L"~all~"}
    };
    const waddle_loader_environment_api_t environment_api = waddle_loader_environment_windows_api();
    waddle_loader_environment_status_t environment_status =
        waddle_loader_environment_begin(&environment, overrides, &environment_api);
    if (environment_status != WaddleLoaderEnvironmentOk) {
        fprintf(stderr, "OpenCL loader environment setup failed status=%d mask=%lu\n",
                (int)environment_status, (unsigned long)environment.restore_failed_mask);
        if (environment.active) restore_loader_environment(&environment);
        return 1;
    }

    native_work_t work = {argc, argv, NULL, NULL, NULL};
    uintptr_t thread = _beginthreadex(NULL, 0, run_native_work, &work, 0, NULL);
    if (!thread) {
        fprintf(stderr, "OpenCL caller thread creation failed: %lu\n", (unsigned long)GetLastError());
        restore_loader_environment(&environment);
        return 1;
    }
    DWORD result = 1;
    if (WaitForSingleObject((HANDLE)thread, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeThread((HANDLE)thread, &result)) abort();
    if (!CloseHandle((HANDLE)thread)) abort();
    if (work.opencl && !FreeLibrary(work.opencl)) result = 1;
    if (work.loader && !FreeLibrary(work.loader)) result = 1;
    if (work.bootstrap && !FreeLibrary(work.bootstrap)) result = 1;
    while (GetModuleHandleW(argv[1]) || GetModuleHandleW(argv[2]) || GetModuleHandleW(argv[4]) ||
           GetModuleHandleW(L"waddle_vulkan_experimental.dll")) {
        result = 1;
        fputs("OpenCL loader environment retained until vendor module retirement\n", stderr);
        fflush(stderr);
        Sleep(10);
    }
    if (restore_loader_environment(&environment)) result = 1;
    if (!result) puts("OpenCL eight exact context lifetimes and native module retirement passed.");
    return (int)result;
}

#include "dxvk_heap_audit_windows.inc"

/** @brief Run the ordinary OpenCL fixture or supervisor-controlled heap cycles.
 * @param[in] argc/argv Borrowed immutable native fixture arguments.
 * @return Zero only after every original OpenCL assertion and optional audit gate.
 * @note Sole main thread. Heap markers bracket fully joined, unloaded cycles.
 */
int wmain(int argc, wchar_t **argv)
{
    if (setvbuf(stdout, NULL, _IONBF, 0) || setvbuf(stderr, NULL, _IONBF, 0)) return 2;
    wchar_t directory[512];
    int enabled = audit_directory(directory, 512);
    if (enabled < 0) return 2;
    if (!enabled) return run_opencl_cycle(argc, argv);

    wchar_t cycles_text[16];
    DWORD length = GetEnvironmentVariableW(L"WADDLE_DXVK_HEAP_CYCLES", cycles_text, 16);
    unsigned cycles = 1;
    if (length) {
        wchar_t *end = NULL;
        unsigned long parsed = wcstoul(cycles_text, &end, 10);
        if (length >= 16 || !parsed || parsed > 8 || !end || *end) return 2;
        cycles = (unsigned)parsed;
    }
    if (audit_stage(directory, "baseline")) return 1;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        if (run_opencl_cycle(argc, argv)) return 1;
        char stage[64];
        int count = snprintf(stage, sizeof stage, "unloaded_%u", cycle + 1);
        if (count <= 0 || (size_t)count >= sizeof stage || audit_stage(directory, stage)) return 1;
    }
    return 0;
}
