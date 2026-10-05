#include "av_ivshmem.h"
#include "av_layout.h"
#include "ivshmem.h"
#include <setupapi.h>
#include <stdlib.h>
void av_ivshmem_free(av_ivshmem_t *memory) {
    if (memory->device) {
        if (memory->mapping) {
            DWORD returned = 0;
            DeviceIoControl(memory->device, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, &returned,
                            NULL);
            memory->mapping = NULL;
        }
        CloseHandle(memory->device);
        memory->device = NULL;
    }
    memory->length = 0;
}
int av_ivshmem_init(av_ivshmem_t *memory, unsigned device_index) {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
        return -1;
    SP_DEVICE_INTERFACE_DATA interface_data = {.cbSize = sizeof(interface_data)};
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
    DWORD needed = 0, returned = 0;
    int result = -1;
    if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_IVSHMEM, device_index,
                                     &interface_data))
        goto cleanup;
    SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, NULL, 0, &needed, NULL);
    if (needed < sizeof(*detail) || needed > 65536)
        goto cleanup;
    detail = calloc(1, needed);
    if (!detail)
        goto cleanup;
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, needed, NULL, NULL))
        goto cleanup;
    HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE)
        goto cleanup;
    memory->device = device;
    IVSHMEM_MMAP_CONFIG config = {.cacheMode = IVSHMEM_CACHE_CACHED};
    IVSHMEM_MMAP mapped = {0};
    if (!DeviceIoControl(device, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(config), &mapped,
                         sizeof(mapped), &returned, NULL))
        goto cleanup;
    memory->mapping = mapped.ptr;
    memory->length = (size_t)mapped.size;
    if (returned != sizeof(mapped) || !mapped.ptr || mapped.size > SIZE_MAX ||
        (uintptr_t)mapped.ptr % 64 || av_layout_validate(mapped.ptr, memory->length) != 0)
        goto cleanup;
    result = 0;
cleanup:
    free(detail);
    detail = NULL;
    SetupDiDestroyDeviceInfoList(devices);
    if (result != 0)
        av_ivshmem_free(memory);
    return result;
}
