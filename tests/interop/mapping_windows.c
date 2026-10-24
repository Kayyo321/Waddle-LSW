/** @file mapping_windows.c
 * @brief Linux/Windows public-API fault fixture for the raw IVSHMEM lease owner.
 *
 * This includes the authoritative production C source once with only its native
 * calls substituted. The returned 2 GiB address is never dereferenced.
 */
#include "interop_mapping_windows.h"
#include "interop_ivshmem_driver.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TestDeviceSet ((HDEVINFO)(uintptr_t)0x1110u)
#define TestDeviceHandle ((HANDLE)(uintptr_t)0x2220u)
#define TestMappingBase ((uintptr_t)0x100000u)
#define TestMappingBytes UINT64_C(0x80000000)

typedef enum failure_t {
    FailureNone,
    FailureOwnerAlloc,
    FailureDetailAlloc,
    FailureGetDeviceSet,
    FailureEnumDevice,
    FailureProbeSuccess,
    FailureProbeWrongError,
    FailureDetailTooSmall,
    FailureDetailTooLarge,
    FailureDetailFetch,
    FailurePathNoTerminator,
    FailurePathEmpty,
    FailureCreateFile,
    FailureRequestMap,
    FailureMapReturnSize,
    FailureMapLength,
    FailureMapNull,
    FailureMapAlignment,
    FailureMapAddressOverflow,
    FailureDestroySet,
    FailureReleaseMap,
    FailureCloseDevice
} failure_t;

typedef struct trace_t {
    size_t get_set_calls;
    size_t enum_calls;
    size_t detail_probe_calls;
    size_t detail_fetch_calls;
    size_t create_file_calls;
    size_t request_calls;
    size_t release_calls;
    size_t close_calls;
    size_t destroy_set_calls;
    size_t get_last_error_calls;
    size_t set_last_error_calls;
    size_t alloc_calls;
    size_t free_calls;
    size_t live_allocations;
    size_t live_bytes;
    size_t live_sets;
    size_t live_devices;
    size_t live_maps;
} trace_t;

static failure_t fail_at;
static failure_t cleanup_fail_at;
static DWORD last_error;
static trace_t trace;
static size_t allocation_attempt;
static void *live_pointers[4];
static size_t live_sizes[4];

static int should_fail(failure_t failure) {
    return fail_at == failure || cleanup_fail_at == failure;
}

static void *mapping_test_calloc(size_t count, size_t size);
static void mapping_test_free(void *pointer);
static DWORD mapping_test_get_last_error(void);
static void mapping_test_set_last_error(DWORD value);
static HDEVINFO mapping_test_get_class_devs(const GUID *class_guid, const WCHAR *enumerator, void *window, DWORD flags);
static BOOL mapping_test_enum_interfaces(HDEVINFO devices, void *device_data, const GUID *class_guid, DWORD index, SP_DEVICE_INTERFACE_DATA *interface_data);
static BOOL mapping_test_get_interface_detail(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA *interface_data, SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail, DWORD detail_bytes, DWORD *needed, void *device_data);
static BOOL mapping_test_destroy_set(HDEVINFO devices);
static HANDLE mapping_test_create_file(const WCHAR *path, DWORD access, DWORD sharing, void *security, DWORD creation, DWORD flags, HANDLE template_file);
static BOOL mapping_test_device_io(HANDLE device, DWORD code, LPVOID input, DWORD input_bytes, LPVOID output, DWORD output_bytes, DWORD *returned, void *overlapped);
static BOOL mapping_test_close_handle(HANDLE handle);

#define calloc mapping_test_calloc
#define free mapping_test_free
#define GetLastError mapping_test_get_last_error
#define SetLastError mapping_test_set_last_error
#define SetupDiGetClassDevsW mapping_test_get_class_devs
#define SetupDiEnumDeviceInterfaces mapping_test_enum_interfaces
#define SetupDiGetDeviceInterfaceDetailW mapping_test_get_interface_detail
#define SetupDiDestroyDeviceInfoList mapping_test_destroy_set
#define CreateFileW mapping_test_create_file
#define DeviceIoControl mapping_test_device_io
#define CloseHandle mapping_test_close_handle
#include "../../src/interop/interop_mapping_windows.c"
#undef calloc
#undef free
#undef GetLastError
#undef SetLastError
#undef SetupDiGetClassDevsW
#undef SetupDiEnumDeviceInterfaces
#undef SetupDiGetDeviceInterfaceDetailW
#undef SetupDiDestroyDeviceInfoList
#undef CreateFileW
#undef DeviceIoControl
#undef CloseHandle

static void reset_trace(void) {
    assert(trace.live_allocations == 0);
    assert(trace.live_bytes == 0);
    assert(trace.live_sets == 0);
    assert(trace.live_devices == 0);
    assert(trace.live_maps == 0);
    memset(&trace, 0, sizeof(trace));
    fail_at = FailureNone;
    cleanup_fail_at = FailureNone;
    last_error = ERROR_SUCCESS;
    allocation_attempt = 0;
    memset(live_pointers, 0, sizeof(live_pointers));
    memset(live_sizes, 0, sizeof(live_sizes));
}

static size_t sdk_call_count(void) {
    return trace.get_set_calls + trace.enum_calls + trace.detail_probe_calls + trace.detail_fetch_calls +
        trace.create_file_calls + trace.request_calls + trace.release_calls + trace.close_calls +
        trace.destroy_set_calls + trace.get_last_error_calls + trace.set_last_error_calls;
}

static void expect_state(
    interop_mapping_windows_t *owner,
    uint32_t resources,
    uint32_t primary,
    uint32_t cleanup) {
    interop_mapping_windows_state_t state = { .owned_resources = 0xAAAAAAAAu, .primary_error = 0xBBBBBBBBu,
                                               .cleanup_error = 0xCCCCCCCCu, .reserved = 0xDDDDDDDDu };
    assert(interop_mapping_windows_get_state(owner, &state) == InteropMappingWindowsOk);
    assert(state.owned_resources == resources);
    assert(state.primary_error == primary);
    assert(state.cleanup_error == cleanup);
    assert(state.reserved == 0);
}

static interop_mapping_windows_t *create_owner(void) {
    interop_mapping_windows_t *owner = NULL;
    assert(interop_mapping_windows_create(&owner) == InteropMappingWindowsOk);
    assert(owner != NULL);
    return owner;
}

static void destroy_empty(interop_mapping_windows_t **owner) {
    assert(interop_mapping_windows_destroy(owner) == InteropMappingWindowsOk);
    assert(*owner == NULL);
    assert(interop_mapping_windows_destroy(owner) == InteropMappingWindowsOk);
    assert(trace.live_allocations == 0);
    assert(trace.live_bytes == 0);
    assert(trace.live_sets == 0);
    assert(trace.live_devices == 0);
    assert(trace.live_maps == 0);
}

static void fill_valid_mapping(IVSHMEM_MMAP *mapping, DWORD *returned) {
    memset(mapping, 0, sizeof(*mapping));
    mapping->peerID = 17;
    mapping->size = TestMappingBytes;
    mapping->ptr = (PVOID)TestMappingBase;
    mapping->vectors = 4;
    *returned = sizeof(*mapping);
    switch (fail_at) {
        case FailureMapReturnSize: *returned = sizeof(*mapping) - 1; break;
        case FailureMapLength: mapping->size = TestMappingBytes - 1; break;
        case FailureMapNull: mapping->ptr = NULL; break;
        case FailureMapAlignment: mapping->ptr = (PVOID)(TestMappingBase + 1); break;
        case FailureMapAddressOverflow: mapping->ptr = (PVOID)(UINTPTR_MAX - UINT64_C(0x7fffffff)); break;
        default: break;
    }
}

static void test_invalid_calls_preserve_outputs_and_make_no_native_calls(void) {
    reset_trace();
    interop_mapping_windows_t *owner = NULL;
    interop_mapping_windows_t *sentinel_owner = (interop_mapping_windows_t *)(uintptr_t)0xCAFEu;
    assert(interop_mapping_windows_create(NULL) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_create(&sentinel_owner) == InteropMappingWindowsInvalidArgument);
    assert(sentinel_owner == (interop_mapping_windows_t *)(uintptr_t)0xCAFEu);
    fail_at = FailureOwnerAlloc;
    assert(interop_mapping_windows_create(&owner) == InteropMappingWindowsOutOfMemory);
    assert(owner == NULL);
    assert(trace.alloc_calls == 1 && trace.live_allocations == 0);

    fail_at = FailureNone;
    owner = create_owner();
    interop_mapping_windows_view_t view = { 1, 2, 3, 4, 5 };
    const interop_mapping_windows_view_t before_view = view;
    assert(interop_mapping_windows_open(NULL, 0, &view) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_open(owner, 0, NULL) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_open(owner, 0, (interop_mapping_windows_view_t *)owner) == InteropMappingWindowsInvalidArgument);
    assert(memcmp(&view, &before_view, sizeof(view)) == 0);
    assert(interop_mapping_windows_close(NULL, 0) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_close(owner, 2) == InteropMappingWindowsInvalidArgument);
    interop_mapping_windows_state_t state = { 1, 2, 3, 4 };
    const interop_mapping_windows_state_t before_state = state;
    assert(interop_mapping_windows_get_state(NULL, &state) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_get_state(owner, NULL) == InteropMappingWindowsInvalidArgument);
    assert(interop_mapping_windows_get_state(owner, (interop_mapping_windows_state_t *)owner) == InteropMappingWindowsInvalidArgument);
    assert(memcmp(&state, &before_state, sizeof(state)) == 0);
    assert(interop_mapping_windows_destroy(NULL) == InteropMappingWindowsInvalidArgument);
    destroy_empty(&owner);
    assert(trace.get_set_calls == 0 && trace.enum_calls == 0 && trace.detail_probe_calls == 0);
    assert(trace.create_file_calls == 0 && trace.request_calls == 0 && trace.release_calls == 0);
    assert(trace.close_calls == 0 && trace.destroy_set_calls == 0);
}

static void run_clean_open_failure(failure_t failure, interop_mapping_windows_status_t expected, DWORD expected_error) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 11, 22, 33, 44, 55 };
    const interop_mapping_windows_view_t before = view;
    fail_at = failure;
    assert(interop_mapping_windows_open(owner, 3, &view) == expected);
    assert(memcmp(&view, &before, sizeof(view)) == 0);
    expect_state(owner, 0, expected_error, 0);
    assert(trace.live_allocations == 1); /* owner only */
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    fail_at = FailureNone;
    destroy_empty(&owner);
}

static void test_acquisition_failures_and_malformed_sdk_data(void) {
    run_clean_open_failure(FailureGetDeviceSet, InteropMappingWindowsOsError, ERROR_GEN_FAILURE);
    run_clean_open_failure(FailureEnumDevice, InteropMappingWindowsOsError, ERROR_NO_MORE_ITEMS);
    run_clean_open_failure(FailureProbeSuccess, InteropMappingWindowsOsError, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureProbeWrongError, InteropMappingWindowsOsError, ERROR_INVALID_FUNCTION);
    run_clean_open_failure(FailureDetailTooSmall, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureDetailTooLarge, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureDetailAlloc, InteropMappingWindowsOutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    run_clean_open_failure(FailureDetailFetch, InteropMappingWindowsOsError, ERROR_INVALID_FUNCTION);
    run_clean_open_failure(FailurePathNoTerminator, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailurePathEmpty, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureCreateFile, InteropMappingWindowsOsError, ERROR_FILE_NOT_FOUND);
    run_clean_open_failure(FailureRequestMap, InteropMappingWindowsOsError, ERROR_INVALID_FUNCTION);
    run_clean_open_failure(FailureMapReturnSize, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureMapLength, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureMapNull, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureMapAlignment, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
    run_clean_open_failure(FailureMapAddressOverflow, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA);
}

static void test_success_busy_missing_proof_and_idempotent_teardown(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 0 };
    assert(interop_mapping_windows_open(owner, 2, &view) == InteropMappingWindowsOk);
    assert(view.mapping_base == TestMappingBase);
    assert(view.mapping_length == TestMappingBytes);
    assert(view.peer_id == 17 && view.vectors == 4 && view.reserved == 0);
    expect_state(owner, InteropMappingWindowsDeviceHandle | InteropMappingWindowsDriverMap, 0, 0);
    assert(trace.live_allocations == 1); /* only the opaque owner is allocated */

    const size_t calls_before = trace.request_calls + trace.release_calls + trace.close_calls + trace.destroy_set_calls + trace.get_set_calls;
    interop_mapping_windows_view_t second_view = { 8, 7, 6, 5, 4 };
    const interop_mapping_windows_view_t before_second = second_view;
    assert(interop_mapping_windows_open(owner, 0, &second_view) == InteropMappingWindowsBusy);
    assert(memcmp(&second_view, &before_second, sizeof(second_view)) == 0);
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsMissingProof);
    assert(interop_mapping_windows_close(owner, 2) == InteropMappingWindowsInvalidArgument);
    assert(trace.request_calls + trace.release_calls + trace.close_calls + trace.destroy_set_calls + trace.get_set_calls == calls_before);
    assert(interop_mapping_windows_destroy(&owner) == InteropMappingWindowsBusy);
    assert(owner != NULL);

    assert(interop_mapping_windows_close(owner, 1) == InteropMappingWindowsOk);
    assert(trace.release_calls == 1 && trace.close_calls == 1 && trace.destroy_set_calls == 1);
    expect_state(owner, 0, 0, 0);
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.release_calls == 1 && trace.close_calls == 1 && trace.destroy_set_calls == 1);
    destroy_empty(&owner);
}

static void test_set_destroy_failure_keeps_unpublished_lease_retryable(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 91, 92, 93, 94, 95 };
    const interop_mapping_windows_view_t before = view;
    fail_at = FailureDestroySet;
    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsCleanupPending);
    assert(memcmp(&view, &before, sizeof(view)) == 0);
    expect_state(owner, InteropMappingWindowsDeviceSet, 0, ERROR_INVALID_FUNCTION);
    assert(trace.release_calls == 1 && trace.close_calls == 1 && trace.destroy_set_calls == 1);
    fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.release_calls == 1 && trace.close_calls == 1 && trace.destroy_set_calls == 2);
    expect_state(owner, 0, 0, 0);
    destroy_empty(&owner);
}

static void test_first_cleanup_error_survives_independent_rollback_failures(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 41, 42, 43, 44, 45 };
    const interop_mapping_windows_view_t before = view;
    fail_at = FailureDestroySet;
    cleanup_fail_at = FailureReleaseMap;

    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsCleanupPending);
    assert(memcmp(&view, &before, sizeof(view)) == 0);
    expect_state(owner, InteropMappingWindowsDeviceSet | InteropMappingWindowsDeviceHandle |
                         InteropMappingWindowsDriverMap,
                 0, ERROR_INVALID_FUNCTION);
    assert(trace.live_sets == 1 && trace.live_devices == 1 && trace.live_maps == 1);
    assert(trace.destroy_set_calls == 1 && trace.release_calls == 1 && trace.close_calls == 0);

    fail_at = FailureNone;
    cleanup_fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.live_sets == 0 && trace.live_devices == 0 && trace.live_maps == 0);
    assert(trace.destroy_set_calls == 2 && trace.release_calls == 2 && trace.close_calls == 1);
    expect_state(owner, 0, 0, 0);
    destroy_empty(&owner);
}

static void test_release_failure_retains_map_and_forbids_close(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 1, 2, 3, 4, 5 };
    const interop_mapping_windows_view_t before = view;
    fail_at = FailureMapLength;
    cleanup_fail_at = FailureReleaseMap;
    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsCleanupPending);
    assert(memcmp(&view, &before, sizeof(view)) == 0);
    expect_state(owner, InteropMappingWindowsDeviceHandle | InteropMappingWindowsDriverMap, ERROR_INVALID_DATA, ERROR_GEN_FAILURE);
    assert(trace.release_calls == 1 && trace.close_calls == 0 && trace.destroy_set_calls == 1);
    fail_at = FailureNone;
    cleanup_fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.release_calls == 2 && trace.close_calls == 1);
    expect_state(owner, 0, ERROR_INVALID_DATA, 0);
    destroy_empty(&owner);
}

static void test_close_failure_retries_without_releasing_twice(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 0 };
    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsOk);
    cleanup_fail_at = FailureCloseDevice;
    assert(interop_mapping_windows_close(owner, 1) == InteropMappingWindowsCleanupPending);
    expect_state(owner, InteropMappingWindowsDeviceHandle, 0, ERROR_GEN_FAILURE);
    assert(trace.release_calls == 1 && trace.close_calls == 1);
    cleanup_fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.release_calls == 1 && trace.close_calls == 2);
    expect_state(owner, 0, 0, 0);
    destroy_empty(&owner);
}

static void test_failed_release_keeps_published_proof_requirement(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 0 };
    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsOk);
    assert(trace.live_sets == 0 && trace.live_devices == 1 && trace.live_maps == 1);

    cleanup_fail_at = FailureReleaseMap;
    assert(interop_mapping_windows_close(owner, 1) == InteropMappingWindowsCleanupPending);
    expect_state(owner, InteropMappingWindowsDeviceHandle | InteropMappingWindowsDriverMap, 0, ERROR_GEN_FAILURE);
    assert(trace.live_sets == 0 && trace.live_devices == 1 && trace.live_maps == 1);
    const size_t calls_after_failed_release = sdk_call_count();
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsMissingProof);
    assert(sdk_call_count() == calls_after_failed_release);
    assert(trace.live_devices == 1 && trace.live_maps == 1);

    cleanup_fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 1) == InteropMappingWindowsOk);
    assert(trace.live_sets == 0 && trace.live_devices == 0 && trace.live_maps == 0);
    assert(trace.release_calls == 2 && trace.close_calls == 1);
    expect_state(owner, 0, 0, 0);
    destroy_empty(&owner);
}

static void test_cleanup_attempts_independent_set_after_device_failure(void) {
    reset_trace();
    interop_mapping_windows_t *owner = create_owner();
    interop_mapping_windows_view_t view = { 1, 2, 3, 4, 5 };
    const interop_mapping_windows_view_t before = view;
    fail_at = FailureEnumDevice;
    cleanup_fail_at = FailureDestroySet;
    assert(interop_mapping_windows_open(owner, 0, &view) == InteropMappingWindowsCleanupPending);
    assert(memcmp(&view, &before, sizeof(view)) == 0);
    expect_state(owner, InteropMappingWindowsDeviceSet, ERROR_NO_MORE_ITEMS, ERROR_INVALID_FUNCTION);
    assert(trace.destroy_set_calls == 1);
    fail_at = FailureNone;
    cleanup_fail_at = FailureNone;
    assert(interop_mapping_windows_close(owner, 0) == InteropMappingWindowsOk);
    assert(trace.destroy_set_calls == 2);
    expect_state(owner, 0, ERROR_NO_MORE_ITEMS, 0);
    destroy_empty(&owner);
}


int main(void) {
    test_invalid_calls_preserve_outputs_and_make_no_native_calls();
    test_acquisition_failures_and_malformed_sdk_data();
    test_success_busy_missing_proof_and_idempotent_teardown();
    test_set_destroy_failure_keeps_unpublished_lease_retryable();
    test_first_cleanup_error_survives_independent_rollback_failures();
    test_release_failure_retains_map_and_forbids_close();
    test_close_failure_retries_without_releasing_twice();
    test_failed_release_keeps_published_proof_requirement();
    test_cleanup_attempts_independent_set_after_device_failure();
    return 0;
}

static void *mapping_test_calloc(size_t count, size_t size) {
    ++trace.alloc_calls;
    ++allocation_attempt;
    if ((fail_at == FailureOwnerAlloc && allocation_attempt == 1) ||
        (fail_at == FailureDetailAlloc && allocation_attempt == 2))
        return NULL;
    void *memory = calloc(count, size);
    if (memory != NULL) {
        ++trace.live_allocations;
        const size_t bytes = count * size;
        trace.live_bytes += bytes;
        size_t slot = 0;
        while (slot < sizeof(live_pointers) / sizeof(live_pointers[0]) && live_pointers[slot] != NULL) ++slot;
        assert(slot < sizeof(live_pointers) / sizeof(live_pointers[0]));
        live_pointers[slot] = memory;
        live_sizes[slot] = bytes;
    }
    return memory;
}

static void mapping_test_free(void *pointer) {
    if (pointer != NULL) {
        assert(trace.live_allocations > 0);
        size_t slot = 0;
        while (slot < sizeof(live_pointers) / sizeof(live_pointers[0]) && live_pointers[slot] != pointer) ++slot;
        assert(slot < sizeof(live_pointers) / sizeof(live_pointers[0]));
        assert(trace.live_bytes >= live_sizes[slot]);
        trace.live_bytes -= live_sizes[slot];
        live_pointers[slot] = NULL;
        live_sizes[slot] = 0;
        --trace.live_allocations;
        ++trace.free_calls;
        free(pointer);
    }
}

static DWORD mapping_test_get_last_error(void) {
    ++trace.get_last_error_calls;
    return last_error;
}
static void mapping_test_set_last_error(DWORD value) {
    ++trace.set_last_error_calls;
    last_error = value;
}

static HDEVINFO mapping_test_get_class_devs(const GUID *class_guid, const WCHAR *enumerator, void *window, DWORD flags) {
    (void)enumerator;
    (void)window;
    ++trace.get_set_calls;
    assert(class_guid == &GUID_DEVINTERFACE_IVSHMEM);
    assert(flags == (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (should_fail(FailureGetDeviceSet)) {
        last_error = ERROR_SUCCESS; /* Exercise zero-error normalization. */
        return INVALID_HANDLE_VALUE;
    }
    ++trace.live_sets;
    return TestDeviceSet;
}

static BOOL mapping_test_enum_interfaces(HDEVINFO devices, void *device_data, const GUID *class_guid, DWORD index, SP_DEVICE_INTERFACE_DATA *interface_data) {
    (void)device_data;
    ++trace.enum_calls;
    assert(devices == TestDeviceSet && class_guid == &GUID_DEVINTERFACE_IVSHMEM);
    assert(index <= UINT32_MAX && interface_data != NULL);
    if (should_fail(FailureEnumDevice)) {
        last_error = ERROR_NO_MORE_ITEMS;
        return FALSE;
    }
    assert(interface_data->cbSize == sizeof(*interface_data));
    return TRUE;
}

static BOOL mapping_test_get_interface_detail(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA *interface_data, SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail, DWORD detail_bytes, DWORD *needed, void *device_data) {
    (void)device_data;
    assert(devices == TestDeviceSet && interface_data != NULL);
    const size_t path_offset = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath);
    const WCHAR path[] = { '\\', '\\', '?', '\\', 'I', 'V', 0 };
    if (detail == NULL) {
        ++trace.detail_probe_calls;
        assert(detail_bytes == 0 && needed != NULL);
        if (fail_at == FailureProbeSuccess) {
            *needed = (DWORD)(path_offset + sizeof(path));
            last_error = ERROR_SUCCESS;
            return TRUE;
        }
        if (fail_at == FailureProbeWrongError) {
            *needed = (DWORD)(path_offset + sizeof(path));
            last_error = ERROR_INVALID_FUNCTION;
            return FALSE;
        }
        if (fail_at == FailureDetailTooSmall) {
            *needed = (DWORD)sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) - 1;
            last_error = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        if (fail_at == FailureDetailTooLarge) {
            *needed = 65537;
            last_error = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        *needed = (DWORD)(path_offset + sizeof(path));
        last_error = ERROR_INSUFFICIENT_BUFFER;
        return FALSE;
    }

    ++trace.detail_fetch_calls;
    assert(needed == NULL && detail_bytes >= path_offset + sizeof(path));
    assert(detail->cbSize == sizeof(*detail));
    if (should_fail(FailureDetailFetch)) {
        last_error = ERROR_INVALID_FUNCTION;
        return FALSE;
    }
    if (fail_at == FailurePathEmpty) {
        detail->DevicePath[0] = 0;
        return TRUE;
    }
    if (fail_at == FailurePathNoTerminator) {
        const size_t chars = (detail_bytes - path_offset) / sizeof(WCHAR);
        for (size_t i = 0; i < chars; ++i) detail->DevicePath[i] = 'X';
        return TRUE;
    }
    memcpy(detail->DevicePath, path, sizeof(path));
    return TRUE;
}

static BOOL mapping_test_destroy_set(HDEVINFO devices) {
    ++trace.destroy_set_calls;
    assert(devices == TestDeviceSet);
    if (should_fail(FailureDestroySet)) {
        last_error = ERROR_INVALID_FUNCTION;
        return FALSE;
    }
    assert(trace.live_sets == 1);
    --trace.live_sets;
    return TRUE;
}

static HANDLE mapping_test_create_file(const WCHAR *path, DWORD access, DWORD sharing, void *security, DWORD creation, DWORD flags, HANDLE template_file) {
    (void)security;
    (void)template_file;
    ++trace.create_file_calls;
    assert(path != NULL && path[0] == '\\' && path[1] == '\\');
    assert(access == (GENERIC_READ | GENERIC_WRITE));
    assert(sharing == (FILE_SHARE_READ | FILE_SHARE_WRITE));
    assert(creation == OPEN_EXISTING && flags == FILE_ATTRIBUTE_NORMAL);
    if (should_fail(FailureCreateFile)) {
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    ++trace.live_devices;
    return TestDeviceHandle;
}

static BOOL mapping_test_device_io(HANDLE device, DWORD code, LPVOID input, DWORD input_bytes, LPVOID output, DWORD output_bytes, DWORD *returned, void *overlapped) {
    (void)overlapped;
    assert(device == TestDeviceHandle && returned != NULL);
    if (code == IOCTL_IVSHMEM_REQUEST_MMAP) {
        ++trace.request_calls;
        assert(input != NULL && input_bytes == 1);
        assert(((IVSHMEM_MMAP_CONFIG *)input)->cacheMode == IVSHMEM_CACHE_CACHED);
        assert(output != NULL && output_bytes == sizeof(IVSHMEM_MMAP));
        if (should_fail(FailureRequestMap)) {
            last_error = ERROR_INVALID_FUNCTION;
            return FALSE;
        }
        fill_valid_mapping((IVSHMEM_MMAP *)output, returned);
        ++trace.live_maps; /* A successful IOCTL owns a map even when its reply is malformed. */
        return TRUE;
    }
    assert(code == IOCTL_IVSHMEM_RELEASE_MMAP);
    ++trace.release_calls;
    assert(input == NULL && input_bytes == 0 && output == NULL && output_bytes == 0);
    assert(trace.live_devices == 1);
    assert(trace.live_maps == 1); /* Reject duplicate release and non-owned release. */
    if (should_fail(FailureReleaseMap)) {
        last_error = ERROR_SUCCESS; /* Exercise cleanup error normalization. */
        return FALSE;
    }
    --trace.live_maps;
    return TRUE;
}

static BOOL mapping_test_close_handle(HANDLE handle) {
    ++trace.close_calls;
    assert(handle == TestDeviceHandle);
    assert(trace.live_devices == 1);
    assert(trace.live_maps == 0); /* The production owner must never close a live mapping. */
    if (should_fail(FailureCloseDevice)) {
        last_error = ERROR_SUCCESS;
        return FALSE;
    }
    --trace.live_devices;
    return TRUE;
}
