/** @file session_control_windows_mapping_seam.c
 * @brief Native composition fixture for the production Windows mapping owner.
 *
 * The production owner and resource ledger are included unchanged. Only the
 * SetupAPI discovery and driver IOCTL boundary are substituted; mapping bytes
 * come from a real 2 GiB VirtualAlloc region and are released through the same
 * owner close path.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "interop_ivshmem_driver.h"
#include "interop_mapping_windows.h"

static PVOID volatile native_mapping_base;

static HDEVINFO native_map_get_class_devs(const GUID *class_guid, const WCHAR *enumerator,
    HWND parent, DWORD flags) {
    HANDLE handle;
    (void)class_guid; (void)enumerator; (void)parent; (void)flags;
    handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    return handle == NULL ? INVALID_HANDLE_VALUE : (HDEVINFO)handle;
}
static BOOL native_map_enum_interfaces(HDEVINFO devices, SP_DEVINFO_DATA *device_data,
    const GUID *class_guid, DWORD index, SP_DEVICE_INTERFACE_DATA *interface_data) {
    (void)device_data; (void)class_guid;
    if (devices == NULL || index != 0 || interface_data == NULL) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    interface_data->Flags = 0;
    interface_data->Reserved = 1;
    return TRUE;
}
static BOOL native_map_get_interface_detail(HDEVINFO devices,
    SP_DEVICE_INTERFACE_DATA *interface_data, SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail,
    DWORD detail_bytes, DWORD *needed, SP_DEVINFO_DATA *device_data) {
    static const WCHAR path[] = L"\\\\.\\WaddleInteropCompositionFixture";
    const DWORD required = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(path));
    (void)interface_data; (void)device_data;
    if (devices == NULL || needed == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *needed = required;
    if (detail == NULL || detail_bytes == 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (detail_bytes < required) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(detail->DevicePath, path, sizeof(path));
    return TRUE;
}
static BOOL native_map_destroy_set(HDEVINFO devices) {
    return CloseHandle((HANDLE)devices);
}
static HANDLE native_map_create_file(const WCHAR *path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    HANDLE handle;
    (void)path; (void)access; (void)sharing; (void)security;
    (void)creation; (void)flags; (void)template_file;
    handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    return handle == NULL ? INVALID_HANDLE_VALUE : handle;
}
static BOOL native_map_device_io(HANDLE device, DWORD code, LPVOID input, DWORD input_bytes,
    LPVOID output, DWORD output_bytes, DWORD *returned, LPOVERLAPPED overlapped) {
    (void)input; (void)input_bytes; (void)overlapped;
    if (device == NULL || returned == NULL) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (code == IOCTL_IVSHMEM_REQUEST_MMAP) {
        if (output == NULL || output_bytes < sizeof(IVSHMEM_MMAP) ||
            InterlockedCompareExchangePointer(&native_mapping_base, NULL, NULL) != NULL) {
            SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
        }
        PVOID mapping_base = VirtualAlloc(NULL, (SIZE_T)UINT64_C(0x80000000),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mapping_base == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        InterlockedExchangePointer(&native_mapping_base, mapping_base);
        IVSHMEM_MMAP *mapping = (IVSHMEM_MMAP *)output;
        memset(mapping, 0, sizeof(*mapping));
        mapping->peerID = 2;
        mapping->size = UINT64_C(0x80000000);
        mapping->ptr = mapping_base;
        mapping->vectors = 1;
        *returned = sizeof(*mapping);
        return TRUE;
    }
    if (code == IOCTL_IVSHMEM_RELEASE_MMAP) {
        PVOID mapping_base = InterlockedCompareExchangePointer(&native_mapping_base, NULL, NULL);
        if (mapping_base == NULL || !VirtualFree(mapping_base, 0, MEM_RELEASE)) {
            SetLastError(ERROR_INVALID_ADDRESS); return FALSE;
        }
        InterlockedExchangePointer(&native_mapping_base, NULL);
        *returned = 0;
        return TRUE;
    }
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}
static BOOL native_map_close_handle(HANDLE handle) { return CloseHandle(handle); }

#define SetupDiGetClassDevsW native_map_get_class_devs
#define SetupDiEnumDeviceInterfaces native_map_enum_interfaces
#define SetupDiGetDeviceInterfaceDetailW native_map_get_interface_detail
#define SetupDiDestroyDeviceInfoList native_map_destroy_set
#define CreateFileW native_map_create_file
#define DeviceIoControl native_map_device_io
#define CloseHandle native_map_close_handle
#include "../../src/interop/interop_mapping_windows.c"
#undef SetupDiGetClassDevsW
#undef SetupDiEnumDeviceInterfaces
#undef SetupDiGetDeviceInterfaceDetailW
#undef SetupDiDestroyDeviceInfoList
#undef CreateFileW
#undef DeviceIoControl
#undef CloseHandle

/**
 * @brief Read the actual VirtualAlloc base while the mapping owner is open.
 * @return Zero when no fixture map is owned; otherwise the borrowed 2 GiB base.
 * @note Read-only fixture observation only. The caller must stop/join all
 *       readers before the controller retries proof-gated mapping cleanup.
 */
uintptr_t interop_guest_native_fixture_mapping_base(void) {
    return (uintptr_t)InterlockedCompareExchangePointer(&native_mapping_base, NULL, NULL);
}
