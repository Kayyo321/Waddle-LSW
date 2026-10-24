/**
 * @file interop_mapping_windows.c
 * @brief Retryable owner for one raw, cached layout-v3 IVSHMEM lease.
 * @ownership Opaque owner state is allocated by create and retained until destroy succeeds.
 * @thread_safety All API calls and borrowed-view access require caller serialization.
 */
#include "interop_ivshmem_driver.h"
#include "interop_mapping_windows.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN64) && !defined(InteropMappingWindowsTestApi)
#error "Raw interop IVSHMEM mapping requires the x64 Windows ABI"
#endif

_Static_assert(sizeof(void *) == 8, "layout-v3 Windows mapping requires 64-bit pointers");
_Static_assert(sizeof(IVSHMEM_MMAP_CONFIG) == 1, "pinned IVSHMEM config ABI changed");
_Static_assert(sizeof(IVSHMEM_MMAP) == 32, "pinned x64 IVSHMEM mmap ABI changed");
_Static_assert(offsetof(IVSHMEM_MMAP, peerID) == 0, "pinned IVSHMEM peerID offset changed");
_Static_assert(offsetof(IVSHMEM_MMAP, size) == 8, "pinned IVSHMEM size offset changed");
_Static_assert(offsetof(IVSHMEM_MMAP, ptr) == 16, "pinned IVSHMEM pointer offset changed");
_Static_assert(offsetof(IVSHMEM_MMAP, vectors) == 24, "pinned IVSHMEM vectors offset changed");
_Static_assert(IOCTL_IVSHMEM_REQUEST_MMAP == 0x00222008u, "pinned REQUEST_MMAP code changed");
_Static_assert(IOCTL_IVSHMEM_RELEASE_MMAP == 0x0022200cu, "pinned RELEASE_MMAP code changed");

#define InteropMappingBytes UINT64_C(0x80000000) /**< Exact layout-v3 BAR size. */
#define InteropMappingAlignment ((uintptr_t)64) /**< Raw mapping base alignment. */
#define InteropDetailBytesMax 65536u /**< Maximum SetupAPI detail allocation. */

struct interop_mapping_windows_t {
    HDEVINFO device_set; /**< SetupAPI set retained until its destroy succeeds. */
    HANDLE device_handle; /**< Driver handle retained until CloseHandle succeeds. */
    IVSHMEM_MMAP mapped; /**< Copied map response; pointer is borrowed from driver. */
    uint32_t owned_resources; /**< Canonical flags; driver-map ownership lives here. */
    uint32_t lease_was_published; /**< Proof guard remains set until map release succeeds. */
    DWORD primary_error; /**< First acquisition error, retained through retries. */
    DWORD cleanup_error; /**< First cleanup error from the current cleanup attempt. */
};

static uint32_t resource_flags(const interop_mapping_windows_t *owner) {
    uint32_t flags = 0;
    if (owner->device_set != INVALID_HANDLE_VALUE && owner->device_set != NULL)
        flags |= InteropMappingWindowsDeviceSet;
    if (owner->device_handle != INVALID_HANDLE_VALUE && owner->device_handle != NULL)
        flags |= InteropMappingWindowsDeviceHandle;
    if ((owner->owned_resources & InteropMappingWindowsDriverMap) != 0)
        flags |= InteropMappingWindowsDriverMap;
    return flags;
}

static DWORD normalized_last_error(void) {
    DWORD error_code = GetLastError();
    return error_code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error_code;
}

/* Caller supplies one already-normalized, nonzero acquisition error per open attempt. */
static void record_primary(interop_mapping_windows_t *owner, DWORD error_code) {
    owner->primary_error = error_code;
}

/* Caller supplies an already-normalized, nonzero cleanup error; preserve the first one. */
static void record_cleanup(interop_mapping_windows_t *owner, DWORD error_code) {
    if (owner->cleanup_error == ERROR_SUCCESS)
        owner->cleanup_error = error_code;
}

static int ranges_overlap(const void *left, size_t left_bytes, const void *right, size_t right_bytes) {
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    if (left_bytes > UINTPTR_MAX - left_start || right_bytes > UINTPTR_MAX - right_start)
        return 1;
    const uintptr_t left_end = left_start + left_bytes;
    const uintptr_t right_end = right_start + right_bytes;
    return left_start < right_end && right_start < left_end;
}

static int device_set_owned(const interop_mapping_windows_t *owner) {
    return owner->device_set != INVALID_HANDLE_VALUE && owner->device_set != NULL;
}

static int device_handle_owned(const interop_mapping_windows_t *owner) {
    return owner->device_handle != INVALID_HANDLE_VALUE && owner->device_handle != NULL;
}

static int driver_map_owned(const interop_mapping_windows_t *owner) {
    return (owner->owned_resources & InteropMappingWindowsDriverMap) != 0;
}

static int try_release_map(interop_mapping_windows_t *owner) {
    if (!driver_map_owned(owner))
        return 1;
    DWORD returned = 0;
    if (!device_handle_owned(owner) ||
        !DeviceIoControl(owner->device_handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0,
                         &returned, NULL)) {
        record_cleanup(owner, device_handle_owned(owner) ? normalized_last_error() : ERROR_INVALID_HANDLE);
        return 0;
    }
    owner->owned_resources &= ~((uint32_t)InteropMappingWindowsDriverMap);
    memset(&owner->mapped, 0, sizeof(owner->mapped));
    owner->lease_was_published = 0;
    return 1;
}

static int try_close_device(interop_mapping_windows_t *owner) {
    if (!device_handle_owned(owner))
        return 1;
    if (driver_map_owned(owner))
        return 0;
    if (!CloseHandle(owner->device_handle)) {
        record_cleanup(owner, normalized_last_error());
        return 0;
    }
    owner->device_handle = NULL;
    owner->owned_resources &= ~((uint32_t)InteropMappingWindowsDeviceHandle);
    return 1;
}

static int try_destroy_device_set(interop_mapping_windows_t *owner) {
    if (!device_set_owned(owner))
        return 1;
    if (!SetupDiDestroyDeviceInfoList(owner->device_set)) {
        record_cleanup(owner, normalized_last_error());
        return 0;
    }
    owner->device_set = INVALID_HANDLE_VALUE;
    owner->owned_resources &= ~((uint32_t)InteropMappingWindowsDeviceSet);
    return 1;
}

static void cleanup_resources(interop_mapping_windows_t *owner, int destroy_device_set) {
    const int map_released = try_release_map(owner);
    if (map_released)
        (void)try_close_device(owner);
    if (destroy_device_set)
        (void)try_destroy_device_set(owner);
}

static interop_mapping_windows_status_t open_failure(
    interop_mapping_windows_t *owner,
    interop_mapping_windows_status_t failure_status,
    DWORD error_code,
    int destroy_device_set) {
    record_primary(owner, error_code);
    cleanup_resources(owner, destroy_device_set);
    owner->owned_resources = resource_flags(owner);
    if (resource_flags(owner) != 0)
        return InteropMappingWindowsCleanupPending;
    return failure_status;
}

static int detail_path_has_bounded_nul(
    const SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail,
    DWORD bytes) {
    const size_t path_offset = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath);
    if ((size_t)bytes < path_offset + sizeof(WCHAR))
        return 0;
    const size_t characters = ((size_t)bytes - path_offset) / sizeof(WCHAR);
    for (size_t index = 0; index < characters; ++index) {
        if (detail->DevicePath[index] == L'\0')
            return index != 0;
    }
    return 0;
}

interop_mapping_windows_status_t interop_mapping_windows_create(
    interop_mapping_windows_t **owner_out) {
    if (owner_out == NULL || *owner_out != NULL)
        return InteropMappingWindowsInvalidArgument;
    interop_mapping_windows_t *owner = calloc(1, sizeof(*owner));
    if (owner == NULL)
        return InteropMappingWindowsOutOfMemory;
    owner->device_set = INVALID_HANDLE_VALUE;
    owner->device_handle = NULL;
    *owner_out = owner;
    return InteropMappingWindowsOk;
}

interop_mapping_windows_status_t interop_mapping_windows_open(
    interop_mapping_windows_t *owner,
    uint32_t device_index,
    interop_mapping_windows_view_t *view_out) {
    if (owner == NULL || view_out == NULL ||
        ranges_overlap(owner, sizeof(*owner), view_out, sizeof(*view_out)))
        return InteropMappingWindowsInvalidArgument;
    if (resource_flags(owner) != 0)
        return InteropMappingWindowsBusy;

    owner->primary_error = ERROR_SUCCESS;
    owner->cleanup_error = ERROR_SUCCESS;
    owner->lease_was_published = 0;
    memset(&owner->mapped, 0, sizeof(owner->mapped));

    HDEVINFO devices = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_IVSHMEM, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return open_failure(owner, InteropMappingWindowsOsError, normalized_last_error(), 0);
    }
    owner->device_set = devices;
    owner->owned_resources |= InteropMappingWindowsDeviceSet;

    SP_DEVICE_INTERFACE_DATA interface_data;
    memset(&interface_data, 0, sizeof(interface_data));
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_IVSHMEM,
                                     device_index, &interface_data)) {
        return open_failure(owner, InteropMappingWindowsOsError, normalized_last_error(), 1);
    }

    DWORD needed = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL probe_ok = SetupDiGetDeviceInterfaceDetailW(
        devices, &interface_data, NULL, 0, &needed, NULL);
    const DWORD probe_error = probe_ok ? ERROR_SUCCESS : normalized_last_error();
    if (probe_ok || probe_error != ERROR_INSUFFICIENT_BUFFER) {
        return open_failure(owner, InteropMappingWindowsOsError,
                            probe_ok ? ERROR_INVALID_DATA : probe_error, 1);
    }
    if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed > InteropDetailBytesMax) {
        return open_failure(owner, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA, 1);
    }

    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = calloc(1, needed);
    if (detail == NULL)
        return open_failure(owner, InteropMappingWindowsOutOfMemory, ERROR_NOT_ENOUGH_MEMORY, 1);
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, needed, NULL, NULL)) {
        const DWORD error_code = normalized_last_error();
        free(detail);
        detail = NULL;
        return open_failure(owner, InteropMappingWindowsOsError, error_code, 1);
    }
    if (!detail_path_has_bounded_nul(detail, needed)) {
        free(detail);
        detail = NULL;
        return open_failure(owner, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA, 1);
    }

    HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        const DWORD error_code = normalized_last_error();
        free(detail);
        detail = NULL;
        return open_failure(owner, InteropMappingWindowsOsError, error_code, 1);
    }
    owner->device_handle = device;
    owner->owned_resources |= InteropMappingWindowsDeviceHandle;
    free(detail);
    detail = NULL;

    const IVSHMEM_MMAP_CONFIG config = {.cacheMode = IVSHMEM_CACHE_CACHED};
    IVSHMEM_MMAP mapped;
    memset(&mapped, 0, sizeof(mapped));
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_IVSHMEM_REQUEST_MMAP, (LPVOID)&config,
                         sizeof(config), &mapped, sizeof(mapped), &returned, NULL)) {
        return open_failure(owner, InteropMappingWindowsOsError, normalized_last_error(), 1);
    }
    owner->mapped = mapped;
    owner->owned_resources |= InteropMappingWindowsDriverMap;

    const uintptr_t base = (uintptr_t)mapped.ptr;
    if (returned != sizeof(mapped) || mapped.size != InteropMappingBytes || mapped.ptr == NULL ||
        base % InteropMappingAlignment != 0 || mapped.size > UINTPTR_MAX - base) {
        return open_failure(owner, InteropMappingWindowsInvalidMapping, ERROR_INVALID_DATA, 1);
    }

    if (!SetupDiDestroyDeviceInfoList(owner->device_set)) {
        const DWORD error_code = normalized_last_error();
        record_cleanup(owner, error_code);
        cleanup_resources(owner, 0);
        owner->owned_resources = resource_flags(owner);
        if (owner->owned_resources != 0)
            return InteropMappingWindowsCleanupPending;
        return InteropMappingWindowsOsError;
    }
    owner->device_set = INVALID_HANDLE_VALUE;
    owner->owned_resources &= ~((uint32_t)InteropMappingWindowsDeviceSet);

    interop_mapping_windows_view_t view = {
        .mapping_base = base,
        .mapping_length = mapped.size,
        .peer_id = mapped.peerID,
        .vectors = mapped.vectors,
        .reserved = 0,
    };
    *view_out = view;
    owner->lease_was_published = 1;
    return InteropMappingWindowsOk;
}

interop_mapping_windows_status_t interop_mapping_windows_close(
    interop_mapping_windows_t *owner,
    uint32_t detached_proof) {
    if (owner == NULL || detached_proof > 1)
        return InteropMappingWindowsInvalidArgument;
    if (owner->lease_was_published && driver_map_owned(owner) && detached_proof != 1)
        return InteropMappingWindowsMissingProof;

    owner->cleanup_error = ERROR_SUCCESS;
    cleanup_resources(owner, 1);
    owner->owned_resources = resource_flags(owner);
    return owner->owned_resources == 0 ? InteropMappingWindowsOk : InteropMappingWindowsCleanupPending;
}

interop_mapping_windows_status_t interop_mapping_windows_get_state(
    const interop_mapping_windows_t *owner,
    interop_mapping_windows_state_t *state_out) {
    if (owner == NULL || state_out == NULL ||
        ranges_overlap(owner, sizeof(*owner), state_out, sizeof(*state_out)))
        return InteropMappingWindowsInvalidArgument;
    const interop_mapping_windows_state_t state = {
        .owned_resources = resource_flags(owner),
        .primary_error = owner->primary_error,
        .cleanup_error = owner->cleanup_error,
        .reserved = 0,
    };
    *state_out = state;
    return InteropMappingWindowsOk;
}

interop_mapping_windows_status_t interop_mapping_windows_destroy(
    interop_mapping_windows_t **owner_inout) {
    if (owner_inout == NULL)
        return InteropMappingWindowsInvalidArgument;
    if (*owner_inout == NULL)
        return InteropMappingWindowsOk;
    interop_mapping_windows_t *owner = *owner_inout;
    if (resource_flags(owner) != 0)
        return InteropMappingWindowsBusy;
    free(owner);
    *owner_inout = NULL;
    return InteropMappingWindowsOk;
}
