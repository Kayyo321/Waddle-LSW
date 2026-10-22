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
static void fail_closed(const char *message);

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

/** @brief Wait a bounded period for this scenario's trusted retirement proof.
 * @param[in] path Fresh trusted UTF-16 receipt path, borrowed.
 * @param[in] identity Exact nonzero authenticated session identity.
 * @param[in] timeout_ms Positive total wait budget.
 * @note Timeout terminates the test process without calling abandon or
 * gracefully unloading modules. The supervisor must still reap its receiver.
 */
static void wait_retired_bounded(const wchar_t *path, uint64_t identity,
                                 ULONGLONG timeout_ms)
{
    ULONGLONG started = GetTickCount64();
    char expected[64];
    int length = snprintf(expected, sizeof expected,
        "retired_session=%016llx\n", (unsigned long long)identity);
    if (length <= 0 || (size_t)length >= sizeof expected)
        fail_closed("OpenCL transport-loss retirement identity formatting failed");
    for (;;) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information = {0};
            char actual[65];
            DWORD count = 0;
            int match = GetFileType(file) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(file, &information) &&
                !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                    FILE_ATTRIBUTE_DIRECTORY)) && information.nNumberOfLinks == 1 &&
                !information.nFileSizeHigh && information.nFileSizeLow == (DWORD)length &&
                ReadFile(file, actual, sizeof actual, &count, NULL) &&
                count == (DWORD)length && !memcmp(actual, expected, (size_t)length);
            if (!CloseHandle(file))
                fail_closed("OpenCL transport-loss retirement receipt handle close failed");
            if (match) return;
        }
        if (GetTickCount64() - started >= timeout_ms)
            fail_closed("OpenCL transport-loss retirement proof timed out; retained session cannot be safely abandoned");
        Sleep(10);
    }
}

/** @brief Fail closed without running CRT or DLL teardown while a worker may be live.
 * @param[in] message Nonnull diagnostic, borrowed until flushed.
 * @note Never returns. The external supervisor remains responsible for actual
 * receiver retirement; process termination is not treated as retirement proof.
 */
static void fail_closed(const char *message)
{
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
    (void)TerminateProcess(GetCurrentProcess(), 1);
    for (;;) Sleep(1000);
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

/** @brief Single dispatch for the separately supervised transport-loss case. */
static const char TransportLossKernel[] =
    "__kernel void transport_loss(__global uint *out){out[0]=0x51a7u;}";

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

#include "opencl_reload_windows.inc"
#include "opencl_os_preinit_windows.inc"

/** @brief Read the diagnostic-only OS pre-initialization switch for heap-audit mode.
 * @return 0 when WADDLE_OPENCL_OS_PREINIT is absent, 1 when it is exactly "1", and -1
 *         for any other value (including empty). Reads the environment only; no owner.
 * @note Sole main thread. Heap qualification runs never set it unless separately approved.
 */
static int audit_os_preinit_requested(void)
{
    wchar_t value[4];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"WADDLE_OPENCL_OS_PREINIT", value, 4);
    if (!length) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : -1;
    return length == 1 && value[0] == L'1' ? 1 : -1;
}

/** @brief One complete exact lifetime on a pre-load raw caller; borrows until join. */
static int run_reload_context(void *opaque) {
    const context_call_t *call=opaque;
    return run_lifetime(call->api,call->device,call->group,NULL);
}

/** @brief Caller-owned state for one blocking OpenCL transport-loss operation.
 * The API thread writes status and event fields; the parent reads them only
 * after WaitForSingleObject and GetExitCodeThread prove natural thread exit.
 */
typedef struct transport_loss_call_t {
    const api_t *api;
    cl_command_queue queue;
    cl_kernel kernel;
    cl_mem buffer;
    cl_uint output;
    cl_event kernel_event;
    cl_int enqueue_status;
    cl_int read_status;
} transport_loss_call_t;

/** @brief Enqueue the real kernel, then block inside clEnqueueReadBuffer.
 * @param[in,out] opaque Parent-owned state kept live through join.
 * @return Zero only if enqueue succeeded and the blocking call returned; the
 * exact OpenCL error is copied to read_status for the joining parent.
 * @note This thread does not log a request witness. Only the bridge's decoded
 * RequestSubmit/opcode-18 marker establishes the actual transport boundary.
 */
static unsigned __stdcall run_transport_loss_call(void *opaque)
{
    transport_loss_call_t *call = opaque;
    size_t one = 1;
    call->enqueue_status = call->api->enqueue_kernel(
        call->queue, call->kernel, 1, NULL, &one, &one, 0, NULL,
        &call->kernel_event);
    if (call->enqueue_status != CL_SUCCESS) return 1;
    call->read_status = call->api->read_buffer(
        call->queue, call->buffer, CL_TRUE, 0, sizeof call->output,
        &call->output, 0, NULL, NULL);
    return 0;
}

/** @brief Release local OpenCL owners in reverse dependency order.
 * @param[in] api Nonnull borrowed API table whose module remains mapped.
 * @param[in,out] event, kernel, buffer, program, queue, context Owned handles;
 * each is nulled after its release call even when the API reports an error.
 * @return Zero only if every release returned CL_SUCCESS.
 * @note Call after transport receipt but before ICD abandon in the fault case.
 * Vulkan dispatch must remain live through SDK destructors and shutdown. The
 * queue must have no outstanding API caller or command group.
 */
static int release_transport_loss_owners(const api_t *api, cl_event *event,
    cl_kernel *kernel, cl_mem *buffer, cl_program *program,
    cl_command_queue *queue, cl_context *context)
{
    int failed = 0;
    if (*event) { if (api->release_event(*event) != CL_SUCCESS) failed = 1; *event = NULL; }
    if (*kernel) { if (api->release_kernel(*kernel) != CL_SUCCESS) failed = 1; *kernel = NULL; }
    if (*buffer) { if (api->release_buffer(*buffer) != CL_SUCCESS) failed = 1; *buffer = NULL; }
    if (*program) { if (api->release_program(*program) != CL_SUCCESS) failed = 1; *program = NULL; }
    if (*queue) { if (api->release_queue(*queue) != CL_SUCCESS) failed = 1; *queue = NULL; }
    if (*context) { if (api->release_context(*context) != CL_SUCCESS) failed = 1; *context = NULL; }
    return failed;
}

/** @brief Shut down clvk while the loader's Vulkan dispatch objects remain live.
 * @param[in] api Borrowed live dispatch, including get_platform_ids.
 * @param[in] shutdown Borrowed private shutdown function; module remains mapped.
 * @note Serial caller after all CL owners retire. Failure terminates without
 * abandoning the ICD or unloading beneath retained SDK state. Five-second bound.
 */
static void shutdown_transport_loss(const api_t *api, clvk_shutdown_t shutdown)
{
    ULONGLONG started = GetTickCount64();
    int32_t status;
    while ((status = shutdown(ClvkShutdownAbi)) == ClvkShutdownBusy) {
        if (GetTickCount64() - started >= 5000)
            fail_closed("OpenCL transport-loss shutdown remained Busy after owner release");
        Sleep(1);
    }
    if (status != ClvkShutdownOk || shutdown(ClvkShutdownAbi) != ClvkShutdownOk)
        fail_closed("OpenCL transport-loss terminal shutdown failed");
    cl_uint count = 0;
    if (api->get_platform_ids(0, NULL, &count) != CL_INVALID_PLATFORM)
        fail_closed("OpenCL transport-loss shutdown did not invalidate platform");
}

/** @brief Release the CL graph after exact receiver reap, before ICD abandonment.
 * @param[in] api/shutdown Borrowed SDK dispatch and shutdown; modules stay mapped.
 * @param[in] receipt Nonnull trusted fresh exact-session receiver-reap receipt.
 * @param[in] identity Nonzero retained bootstrap session; never substituted.
 * @param[in] abandon Borrowed bootstrap call, invoked only after SDK shutdown.
 * @param[in,out] event/kernel/buffer/program/queue/context Owned handles, nulled
 * by reverse release. Their Vulkan dispatch objects must remain valid through
 * every destructor and terminal SDK shutdown; abandon clears those objects.
 * @return One if any CL release failed, zero otherwise. Receipt/shutdown/abandon
 * failure terminates fail-closed without claiming graceful module retirement.
 * @note Sole joined caller. No application API call can overlap this sequence.
 */
static int release_transport_loss_after_retirement(const api_t *api,
    clvk_shutdown_t shutdown, const wchar_t *receipt, uint64_t identity,
    bootstrap_abandon_t abandon, cl_event *event, cl_kernel *kernel,
    cl_mem *buffer, cl_program *program, cl_command_queue *queue,
    cl_context *context)
{
    wait_retired_bounded(receipt, identity, 90000);
    int failed = release_transport_loss_owners(api, event, kernel, buffer,
                                              program, queue, context);
    shutdown_transport_loss(api, shutdown);
    if (abandon(identity) != RingOk)
        fail_closed("OpenCL transport-loss session abandonment failed after SDK shutdown");
    return failed;
}

/** @brief Exercise one real buffer-backed kernel cancellation and retirement.
 * @param[in] api Nonnull borrowed dispatch; module outlives all owners.
 * @param[in] device Borrowed selected RTX 5080 device.
 * @param[in] receipt Trusted fresh retirement receipt path.
 * @param[in] stop/session/abandon Borrowed serialized bootstrap lifecycle calls.
 * @return Zero only for exact enqueue/read result and complete post-proof teardown.
 * @note The supervisor must observe its fresh RequestSubmit/opcode-18 marker,
 * cancel that session, reap the exact worker, and publish the matching receipt.
 * SDK release and shutdown precede ICD abandon to preserve loader dispatch.
 */
static int run_transport_loss(const api_t *api, cl_device_id device,
    clvk_shutdown_t shutdown, const wchar_t *receipt, bootstrap_stop_t stop,
    bootstrap_session_t session, bootstrap_abandon_t abandon)
{
    int failed = 0;
    int read_started = 0;
    int api_thread_started = 0;
    cl_int status = CL_SUCCESS;
    cl_context context = NULL;
    cl_command_queue queue = NULL;
    cl_program program = NULL;
    cl_kernel kernel = NULL;
    cl_mem buffer = NULL;
    transport_loss_call_t call = {0};
    HANDLE thread = NULL;
    uint64_t identity = session();
    if (!identity) fail_closed("OpenCL transport-loss session identity missing");

    context = api->create_context(NULL, 1, &device, NULL, NULL, &status);
    if (!context || status != CL_SUCCESS) goto setup_failure;
    queue = api->create_queue(context, device, 0, &status);
    if (!queue || status != CL_SUCCESS) goto setup_failure;
    const char *source = TransportLossKernel;
    program = api->create_program(context, 1, &source, NULL, &status);
    if (!program || status != CL_SUCCESS) goto setup_failure;
    if (api->build_program(program, 1, &device, "", NULL, NULL) != CL_SUCCESS)
        goto setup_failure;
    kernel = api->create_kernel(program, "transport_loss", &status);
    if (!kernel || status != CL_SUCCESS) goto setup_failure;
    buffer = api->create_buffer(context, CL_MEM_READ_WRITE, sizeof(cl_uint), NULL, &status);
    if (!buffer || status != CL_SUCCESS) goto setup_failure;
    if (api->set_arg(kernel, 0, sizeof buffer, &buffer) != CL_SUCCESS)
        goto setup_failure;

    call.api = api;
    call.queue = queue;
    call.kernel = kernel;
    call.buffer = buffer;
    call.enqueue_status = CL_INVALID_OPERATION;
    call.read_status = CL_INVALID_OPERATION;
    fprintf(stderr, "OpenCL transport-loss call armed session=%016llx; supervisor must witness bridge RequestSubmit opcode=18 (API entry is not the witness)\n",
            (unsigned long long)identity);
    fflush(stderr);
    uintptr_t raw_thread = _beginthreadex(NULL, 0, run_transport_loss_call,
                                         &call, 0, NULL);
    if (!raw_thread) fail_closed("OpenCL transport-loss API thread creation failed");
    api_thread_started = 1;
    thread = (HANDLE)raw_thread;
    DWORD wait_status = WaitForSingleObject(thread, 65000);
    if (wait_status != WAIT_OBJECT_0)
        fail_closed("OpenCL transport-loss API call did not finish within 65 seconds");
    DWORD thread_code = 1;
    if (!GetExitCodeThread(thread, &thread_code) || !CloseHandle(thread))
        fail_closed("OpenCL transport-loss API thread join/close failed");
    thread = NULL;
    read_started = thread_code == 0 && call.enqueue_status == CL_SUCCESS;
    fprintf(stderr, "OpenCL transport-loss API thread joined exit=%lu enqueue_status=%d read_status=%d output=%u\n",
            (unsigned long)thread_code, (int)call.enqueue_status,
            (int)call.read_status, (unsigned)call.output);
    fflush(stderr);
    if (shutdown(ClvkShutdownAbi) != ClvkShutdownBusy)
        fail_closed("OpenCL shutdown was not Busy while transport-loss owners remained live");

    if (!read_started) goto setup_failure;
    if (call.read_status != CL_OUT_OF_RESOURCES) failed = 1;

    goto retire_state;

setup_failure:
    failed = 1;
retire_state:
    if (api_thread_started) {
        /* Even an enqueue failure may follow a real submit. Retain all owners
         * until exact receiver reap, then keep dispatch valid through SDK teardown. */
        if (release_transport_loss_after_retirement(api, shutdown, receipt,
                identity, abandon, &call.kernel_event, &kernel, &buffer,
                &program, &queue, &context))
            failed = 1;
    } else {
        /* No API worker submitted work. Destroy the SDK graph before normal
         * bootstrap stop; a failed stop still needs exact receiver-reap proof. */
        if (release_transport_loss_owners(api, &call.kernel_event, &kernel,
                &buffer, &program, &queue, &context))
            failed = 1;
        shutdown_transport_loss(api, shutdown);
        if (stop() != RingOk) {
            wait_retired_bounded(receipt, identity, 90000);
            if (abandon(identity) != RingOk)
                fail_closed("OpenCL setup failure abandonment failed after SDK shutdown");
        }
    }
    return failed;
}

/** @brief Joined caller-thread state; modules transfer to the joining parent.
 * argc/argv are borrowed until join; only the worker writes module references.
 */
typedef struct native_work_t {
    int argc;
    wchar_t **argv;
    int transport_loss;
    reload_cycle_result_t *reload; /* Optional parent-owned output; survives worker join. */
    HMODULE bootstrap;
    HMODULE loader;
    HMODULE opencl;
} native_work_t;

/** @brief Execute one module graph; all paths borrowed from the supervisor.
 * @param[in] argc/argv Exactly seven: executable, loader, OpenCL.dll, manifest,
 * bootstrap DLL, private config, fresh retirement receipt. Existing inputs use absolute drive paths.
 * @return Zero after eight ordinary contexts or three reload contexts and
 * natural bootstrap stop, otherwise
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
    const int base = work->transport_loss ? 2 : 1;
    if ((!work->transport_loss && argc != 7) ||
        (work->transport_loss && (argc != 8 || wcscmp(argv[1], L"--transport-loss")))) return 2;
    for (int i = base; i < base + 5; ++i)
        if (wcslen(argv[i]) < 3 || argv[i][1] != L':' || argv[i][2] != L'\\' ||
            GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) return 2;
    if (wcslen(argv[base + 5]) < 3 || argv[base + 5][1] != L':' || argv[base + 5][2] != L'\\' ||
        GetFileAttributesW(argv[base + 5]) != INVALID_FILE_ATTRIBUTES) return 2;
    int result = 1;
    HMODULE bootstrap = NULL, loader = NULL, opencl = NULL;
    api_t api = {0};
    cl_uint count = 0;
    bootstrap_start_t start = NULL;
    bootstrap_stop_t stop = NULL;
    bootstrap_session_t session = NULL;
    bootstrap_abandon_t abandon = NULL;
    clvk_shutdown_t shutdown = NULL;
    reload_pool_t foreign={0};
    if(work->reload && reload_pool_start(&foreign,ReloadForeignThreads))goto cleanup;
    bootstrap = LoadLibraryExW(argv[base + 3], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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
    int config_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[base + 4], -1,
                                          config, sizeof config, NULL, NULL);
    if (!config_bytes) goto cleanup;
    ULONGLONG load_started = GetTickCount64();
    loader = LoadLibraryExW(argv[base], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) goto cleanup;
    opencl = LoadLibraryExW(argv[base + 1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!opencl) goto cleanup;
    if(work->reload) {
        if(!reload_module_exact(opencl,argv[base+1]) || !reload_module_exact(loader,argv[base]) ||
           !reload_module_exact(bootstrap,argv[base+3]))goto cleanup;
        work->reload->module_exact=1;
    }
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
    if(work->reload)work->reload->session=session();
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
    if (work->transport_loss) {
        result = run_transport_loss(&api, device, shutdown, argv[base + 5],
                                    stop, session, abandon);
        work->opencl = opencl;
        work->loader = loader;
        work->bootstrap = bootstrap;
        return (unsigned)result;
    }
    if(work->reload) {
        if(run_lifetime(&api,device,group,shutdown))goto cleanup;
        context_call_t call={&api,device,group};
        for(unsigned i=0;i<ReloadForeignThreads;++i)work->reload->foreign[i]=foreign.slots[i].tid;
        if(reload_pool_call(&foreign,run_reload_context,&call))goto cleanup;
        puts("OpenCL reload three exact context lifetimes and raw pre-load callers joined.");fflush(stdout);
        goto workload_done;
    }
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
workload_done:
    result = 0;
cleanup:
    if(work->reload && reload_pool_finish(&foreign))result=1;
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
            wait_retired(argv[base + 5], retained);
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
 * @param[out] reload Optional caller-owned result, valid only on success;
 * storage remains live until the joined worker and module graph retire.
 * @return Zero only after exact kernels, terminal shutdown, natural host stop,
 * joined worker and complete module retirement. Main thread owns the thread
 * handle and transferred module references and closes/releases each once.
 * The parent-owned loader snapshot remains active until all vendor modules exit.
 */
static int run_opencl_cycle_mode(int argc, wchar_t **argv,reload_cycle_result_t *reload) {
    const int transport_loss = argc > 1 && !wcscmp(argv[1], L"--transport-loss");
    const int base = transport_loss ? 2 : 1;
    if ((!transport_loss && argc != 7) || (transport_loss && argc != 8)) return 2;
    for (int i = base; i < base + 5; ++i)
        if (wcslen(argv[i]) < 3 || argv[i][1] != L':' || argv[i][2] != L'\\' ||
            GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES) return 2;
    if (wcslen(argv[base + 5]) < 3 || argv[base + 5][1] != L':' || argv[base + 5][2] != L'\\' ||
        GetFileAttributesW(argv[base + 5]) != INVALID_FILE_ATTRIBUTES) return 2;

    waddle_loader_environment_guard_t environment;
    waddle_loader_environment_guard_init(&environment);
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount] = {
        {TRUE, argv[base + 2]}, {FALSE, NULL}, {FALSE, NULL}, {TRUE, L"~all~"}
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

    native_work_t work = {argc, argv, transport_loss, reload, NULL, NULL, NULL};
    uintptr_t thread = _beginthreadex(NULL, 0, run_native_work, &work, 0, NULL);
    if (!thread) {
        fprintf(stderr, "OpenCL caller thread creation failed: %lu\n", (unsigned long)GetLastError());
        restore_loader_environment(&environment);
        return 1;
    }
    DWORD result = 1;
    DWORD thread_wait = WaitForSingleObject((HANDLE)thread,
        transport_loss ? 180000 : reload ? ReloadCycleMs : INFINITE);
    if (thread_wait != WAIT_OBJECT_0 || !GetExitCodeThread((HANDLE)thread, &result)) {
        if(reload)fail_closed("OpenCL reload fatal: outer caller did not join within its deadline");
        if (transport_loss)
            fail_closed("OpenCL transport-loss outer worker did not join within its deadline");
        abort();
    }
    if (!CloseHandle((HANDLE)thread)) abort();
    if (work.opencl && !FreeLibrary(work.opencl)) result = 1;
    if (work.loader && !FreeLibrary(work.loader)) result = 1;
    if (work.bootstrap && !FreeLibrary(work.bootstrap)) result = 1;
    ULONGLONG module_wait_started = GetTickCount64();
    while (GetModuleHandleW(argv[base]) || GetModuleHandleW(argv[base + 1]) || GetModuleHandleW(argv[base + 3]) ||
           GetModuleHandleW(L"waddle_vulkan_experimental.dll")) {
        if(reload && GetTickCount64()-module_wait_started>=10000)
            fail_closed("OpenCL reload fatal: module retirement timed out");
        if (transport_loss && GetTickCount64() - module_wait_started >= 10000)
            fail_closed("OpenCL transport-loss module retirement timed out");
        result = 1;
        fputs("OpenCL loader environment retained until vendor module retirement\n", stderr);
        fflush(stderr);
        Sleep(10);
    }
    if(reload)reload->modules_absent=1;
    if (restore_loader_environment(&environment)) result = 1;
    if (!result && reload)puts("OpenCL reload shutdown, session stop and module retirement passed.");
    if (!result && !reload) puts(transport_loss ?
        "OpenCL transport-loss result, retirement, shutdown, and module retirement passed." :
        "OpenCL eight exact context lifetimes and native module retirement passed.");
    return (int)result;
}

/** @brief Existing ordinary/cancellation entry, with reload observation disabled. */
static int run_opencl_cycle(int argc,wchar_t **argv) {
    return run_opencl_cycle_mode(argc,argv,NULL);
}

#include "dxvk_heap_audit_windows.inc"
#include "native_csr_heap_windows.inc"

/** @brief Parent-owned stress inputs; all borrowed arguments survive the run. */
typedef struct reload_fixture_t { int argc;wchar_t **argv;const wchar_t *directory; } reload_fixture_t;
/** @brief Invoke one fully joined/unloaded real module lifetime. */
static int reload_fixture_cycle(void *opaque,reload_cycle_result_t *result) {
    reload_fixture_t *fixture=opaque;
    return run_opencl_cycle_mode(fixture->argc,fixture->argv,result);
}
/** @brief Read-only graph absence check; returned module handles are borrowed. */
static int reload_fixture_absent(void *opaque) {
    reload_fixture_t *fixture=opaque;wchar_t **argv=fixture->argv;
    return !GetModuleHandleW(argv[1]) && !GetModuleHandleW(argv[2]) && !GetModuleHandleW(argv[4]) &&
        !GetModuleHandleW(L"waddle_vulkan_experimental.dll");
}
/** @brief Existing exact PID/creation/stage handshake; no process owner transfer. */
static int reload_fixture_stage(void *opaque,const char *stage) {
    return audit_stage(((reload_fixture_t *)opaque)->directory,stage);
}
/** @brief Approved OS-only first-use initialization before the entry baseline (amendments 1-2).
 * @return Zero only when all six steps succeeded; owns nothing on return.
 */
static int reload_fixture_preinit(void *opaque) {
    (void)opaque;
    return os_preinitialize();
}

/** @brief Run the ordinary OpenCL fixture or supervisor-controlled heap cycles.
 * @param[in] argc/argv Borrowed immutable native fixture arguments.
 * @return Zero only after every original OpenCL assertion and optional audit gate.
 * @note Sole main thread. Heap markers bracket fully joined, unloaded cycles.
 */
int wmain(int argc, wchar_t **argv)
{
    if (setvbuf(stdout, NULL, _IONBF, 0) || setvbuf(stderr, NULL, _IONBF, 0)) return 2;
    if(argc==2 && !wcscmp(argv[1],L"--runtime-control-tls"))return reload_control(0);
    if(argc==2 && !wcscmp(argv[1],L"--runtime-control-fls"))return reload_control(1);
    wchar_t directory[512];
    int enabled = audit_directory(directory, 512);
    if (enabled < 0) return 2;
    int os_preinit = audit_os_preinit_requested();
    if (os_preinit < 0) return 2;
    if(argc>1 && !wcscmp(argv[1],L"--runtime-reload")) {
        if(argc!=8 || enabled!=1 || os_preinit)return 2;
        wchar_t legacy[2];SetLastError(ERROR_SUCCESS);
        if(GetEnvironmentVariableW(L"WADDLE_DXVK_HEAP_CYCLES",legacy,2) || GetLastError()!=ERROR_ENVVAR_NOT_FOUND)return 2;
        wchar_t *cycle_argv[7]={argv[0],argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]};
        reload_fixture_t fixture={7,cycle_argv,directory};
        const reload_api_t api={reload_fixture_cycle,reload_fixture_absent,reload_fixture_stage,
                                reload_fixture_preinit,OsPreinitRecord,&fixture};
        return reload_run(&api,cycle_argv[2],stdout);
    }
    if (enabled && argc > 1 && !wcscmp(argv[1], L"--transport-loss")) return 2;
    if (!enabled) return os_preinit ? 2 : run_opencl_cycle(argc, argv);

    wchar_t cycles_text[16];
    DWORD length = GetEnvironmentVariableW(L"WADDLE_DXVK_HEAP_CYCLES", cycles_text, 16);
    unsigned cycles = 1;
    if (length) {
        wchar_t *end = NULL;
        unsigned long parsed = wcstoul(cycles_text, &end, 10);
        if (length >= 16 || !parsed || parsed > 8 || !end || *end) return 2;
        cycles = (unsigned)parsed;
    }
    if (os_preinit) {
        if (os_preinitialize()) {
            fputs("OpenCL OS pre-initialization failed\n", stderr);
            return 1;
        }
        puts("OpenCL OS pre-initialization completed: " OsPreinitRecord);
    }
    if (native_csr_stage(directory, "baseline")) return 1;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        if (run_opencl_cycle(argc, argv)) return 1;
        char stage[64];
        int count = snprintf(stage, sizeof stage, "unloaded_%u", cycle + 1);
        if (count <= 0 || (size_t)count >= sizeof stage || native_csr_stage(directory, stage)) return 1;
    }
    return 0;
}
