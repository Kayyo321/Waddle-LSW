/** @file mapping_windows.c
 * @brief Native SDK fixture for signed-driver mapping ownership and failures.
 * Fake driver pages/handles are borrowed from this fixture; this is not evidence
 * of a successful real IVSHMEM driver mapping or Windows guest GPU execution.
 */
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include "venus_driver.h"
#include "waddle/venus_mapping.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned failure_stage;
static unsigned owned_lists, owned_devices, owned_maps, owned_allocations;
static _Alignas(64) uint8_t driver_bytes[4096];

static HDEVINFO fake_get_devices(const GUID *class_id, PCWSTR enumerator,
                                 HWND parent, DWORD flags) {
    (void)class_id; (void)enumerator; (void)parent; (void)flags;
    if (failure_stage == 1) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    ++owned_lists;
    return (HDEVINFO)(uintptr_t)10;
}
static BOOL fake_enumerate(HDEVINFO devices, PSP_DEVINFO_DATA info, const GUID *class_id,
                           DWORD index, PSP_DEVICE_INTERFACE_DATA interface_data) {
    (void)devices; (void)info; (void)class_id; (void)index;
    DWORD bytes = 0;
    memcpy(&bytes, interface_data, sizeof(bytes));
    assert(bytes == sizeof(*interface_data));
    if (failure_stage == 2) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
static BOOL fake_get_detail(HDEVINFO devices, PSP_DEVICE_INTERFACE_DATA interface_data,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD bytes, PDWORD required,
    PSP_DEVINFO_DATA info) {
    (void)devices; (void)interface_data; (void)info;
    if (!detail) {
        *required = failure_stage == 3 ? 0 : failure_stage == 4 ? 65537 : 1024;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    assert(bytes == 1024);
    DWORD detail_bytes = 0;
    memcpy(&detail_bytes, detail, sizeof(detail_bytes));
    assert(detail_bytes == sizeof(*detail));
    if (failure_stage == 6) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    /* The system structure's path field is a vendor ABI identifier. */
    const WCHAR Path[] = {L'x', L'\0'};
    memcpy((uint8_t *)detail + offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath),
           Path, sizeof(Path));
    return TRUE;
}
static HANDLE fake_create_file(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD attributes, HANDLE template_file) {
    (void)access; (void)share; (void)security; (void)disposition;
    (void)attributes; (void)template_file;
    assert(path[0] == L'x');
    if (failure_stage == 7) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    ++owned_devices;
    return (HANDLE)(uintptr_t)20;
}
static BOOL fake_ioctl(HANDLE device, DWORD code, LPVOID input, DWORD input_bytes,
    LPVOID output, DWORD output_bytes, LPDWORD returned, LPOVERLAPPED overlapped) {
    (void)input_bytes; (void)overlapped;
    assert(device == (HANDLE)(uintptr_t)20);
    if (code == IOCTL_IVSHMEM_RELEASE_MMAP) {
        assert(owned_maps == 1);
        --owned_maps;
        *returned = 0;
        return TRUE;
    }
    assert(code == IOCTL_IVSHMEM_REQUEST_MMAP);
    assert(((uint8_t *)input)[0] == IVSHMEM_CACHE_CACHED);
    assert(output_bytes == sizeof(IVSHMEM_MMAP));
    if (failure_stage == 8) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    IVSHMEM_MMAP mapped = {0};
    mapped.ptr = failure_stage == 10 ? NULL : driver_bytes + (failure_stage == 11 ? 1 : 0);
    mapped.size = failure_stage == 12 ? (uint64_t)VenusRegionMaxBytes + 1 : sizeof(driver_bytes);
    memcpy(output, &mapped, sizeof(mapped));
    *returned = failure_stage == 9 ? 0 : sizeof(mapped);
    ++owned_maps;
    return TRUE;
}
static BOOL fake_destroy_devices(HDEVINFO devices) {
    assert(devices == (HDEVINFO)(uintptr_t)10 && owned_lists == 1);
    --owned_lists;
    SetLastError(ERROR_SUCCESS); /* Cleanup must preserve the original failure. */
    return TRUE;
}
static BOOL fake_close(HANDLE device) {
    assert(device == (HANDLE)(uintptr_t)20 && owned_devices == 1 && owned_maps == 0);
    --owned_devices;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
static void *fake_calloc(size_t count, size_t size) {
    if (failure_stage == 5) return NULL;
    void *allocation = calloc(count, size);
    if (allocation) ++owned_allocations;
    return allocation;
}
static void fake_free(void *allocation) {
    if (allocation) { assert(owned_allocations == 1); --owned_allocations; }
    free(allocation);
}

#define SetupDiGetClassDevsW fake_get_devices
#define SetupDiEnumDeviceInterfaces fake_enumerate
#define SetupDiGetDeviceInterfaceDetailW fake_get_detail
#define SetupDiDestroyDeviceInfoList fake_destroy_devices
#define CreateFileW fake_create_file
#define DeviceIoControl fake_ioctl
#define CloseHandle fake_close
#define calloc fake_calloc
#define free fake_free
#include "../../src/vgpu/venus_mapping_windows.c"
#undef SetupDiGetClassDevsW
#undef SetupDiEnumDeviceInterfaces
#undef SetupDiGetDeviceInterfaceDetailW
#undef SetupDiDestroyDeviceInfoList
#undef CreateFileW
#undef DeviceIoControl
#undef CloseHandle
#undef calloc
#undef free

int main(void) {
    venus_mapping_t memory = {0};
    venus_mapping_free(NULL);
    venus_mapping_free(&memory);
    assert(venus_mapping_open(NULL, 0) == -1 && GetLastError() == ERROR_INVALID_PARAMETER);
    for (failure_stage = 0; failure_stage <= 14; ++failure_stage) {
        assert(venus_region_init(driver_bytes, sizeof(driver_bytes), 1024) == RingOk);
        if (failure_stage == 13) driver_bytes[0] ^= 1;
        if (failure_stage == 14) {
            venus_region_view_t view;
            assert(venus_region_attach(&view, driver_bytes, sizeof(driver_bytes)) == RingOk);
            assert(venus_ring_close(&view.commands) == RingOk);
            venus_region_detach(&view);
        }
        int result = venus_mapping_open(&memory, 0);
        if (failure_stage == 0) {
            assert(result == 0 && memory.mapping == driver_bytes);
            uint8_t byte = 0x42, output = 0;
            assert(venus_ring_write(&memory.view.commands, &byte, 1) == RingOk);
            assert(venus_ring_read(&memory.view.commands, &output, 1) == RingOk && output == byte);
            SetLastError(ERROR_RETRY);
            venus_mapping_free(&memory);
            assert(GetLastError() == ERROR_RETRY);
        } else {
            assert(result == -1);
            DWORD expected = failure_stage == 5 ? ERROR_NOT_ENOUGH_MEMORY :
                (failure_stage == 3 || failure_stage == 4 || failure_stage >= 9) ?
                ERROR_INVALID_DATA : ERROR_ACCESS_DENIED;
            assert(GetLastError() == expected);
        }
        assert(!owned_lists && !owned_devices && !owned_maps && !owned_allocations);
        assert(memory.mapping == NULL && memory.mapping_bytes == 0);
        assert(!memory.owns_handle && !memory.owns_mapping && !memory.view.commands.header);
        venus_mapping_free(&memory);
    }
    return 0;
}
