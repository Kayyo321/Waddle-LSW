#include "venus_driver.h"
#include "waddle/venus_mapping.h"
#include <setupapi.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void venus_mapping_free(venus_mapping_t *memory) {
    if (!memory)
        return;
    DWORD saved_error = GetLastError();
    venus_region_detach(&memory->view);
    if (memory->owns_mapping) {
        DWORD returned = 0;
        DeviceIoControl((HANDLE)memory->native_handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0,
                        &returned, NULL);
    }
    if (memory->owns_handle)
        CloseHandle((HANDLE)memory->native_handle);
    memset(memory, 0, sizeof(*memory));
    SetLastError(saved_error);
}

int venus_mapping_open(venus_mapping_t *memory, unsigned device_index) {
    if (!memory) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    memset(memory, 0, sizeof(*memory));
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
        return -1;
    SP_DEVICE_INTERFACE_DATA interface_data = {0};
    const DWORD InterfaceBytes = sizeof(interface_data);
    memcpy(&interface_data, &InterfaceBytes, sizeof(InterfaceBytes));
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
    DWORD needed = 0, returned = 0;
    int result = -1;
    if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_IVSHMEM, device_index,
                                     &interface_data))
        goto cleanup;
    SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, NULL, 0, &needed, NULL);
    if (needed < sizeof(*detail) || needed > 65536) {
        SetLastError(ERROR_INVALID_DATA);
        goto cleanup;
    }
    /* Session thread owns detail until the single cleanup site below. */
    detail = calloc(1, needed);
    if (!detail) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }
    const DWORD DetailBytes = sizeof(*detail);
    memcpy(detail, &DetailBytes, sizeof(DetailBytes));
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, needed, NULL, NULL))
        goto cleanup;
    HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE)
        goto cleanup;
    memory->native_handle = (intptr_t)device;
    memory->owns_handle = 1;
    IVSHMEM_MMAP_CONFIG config = {IVSHMEM_CACHE_CACHED};
    IVSHMEM_MMAP mapped = {0};
    if (!DeviceIoControl(device, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(config), &mapped,
                         sizeof(mapped), &returned, NULL))
        goto cleanup;
    memory->owns_mapping = 1;
    memory->mapping = mapped.ptr;
    memory->mapping_bytes = (size_t)mapped.size;
    if (returned != sizeof(mapped) || !mapped.ptr || mapped.size > VenusRegionMaxBytes ||
        ((uintptr_t)mapped.ptr & 63u) ||
        venus_region_attach(&memory->view, mapped.ptr, memory->mapping_bytes) != RingOk) {
        SetLastError(ERROR_INVALID_DATA);
        goto cleanup;
    }
    result = 0;
cleanup: {
    DWORD saved_error = GetLastError();
    free(detail);
    detail = NULL;
    SetupDiDestroyDeviceInfoList(devices);
    if (result != 0)
        venus_mapping_free(memory);
    SetLastError(saved_error);
    return result;
}
}
