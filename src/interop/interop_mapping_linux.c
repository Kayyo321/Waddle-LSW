/**
 * @file interop_mapping_linux.c
 * @brief Retryable Linux descriptor and mmap ownership for layout v3.
 *
 * This implementation owns only its duplicated descriptor and local mapping.
 * It neither initializes shared bytes nor infers peer lifetime from local
 * cleanup. Every lifecycle call is confined to the pthread that created the
 * owner. OS operations are intentionally small and remain linker-wrappable by
 * isolated tests without adding a production test API.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "interop_mapping_linux.h"

#include "interop_native_atomic.h"
#include "../av/av_kvmfr.h"
#include "waddle/interop_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/** @brief Frozen mapping length. */
#define InteropMappingLinuxBytes UINT64_C(0x80000000)

struct interop_mapping_linux_t {
    pthread_t owner_thread;
    int owned_fd;
    void *mapping_base;
    uint64_t mapping_length;
    interop_mapping_linux_backing_t backing_kind;
    bool mapped;
    bool header_validated;
    bool view_published;
    bool closing;
};

typedef struct pointer_range_t {
    uintptr_t begin;
    uintptr_t end;
} pointer_range_t;

/** @brief Check an address range without wrapping uintptr_t. */
static bool pointer_range(const void *pointer, size_t length, pointer_range_t *range) {
    uintptr_t begin;
    if (pointer == NULL || length == 0 || range == NULL) return false;
    begin = (uintptr_t)pointer;
    if (begin > UINTPTR_MAX - length) return false;
    range->begin = begin;
    range->end = begin + length;
    return true;
}

/** @brief Return true when two half-open address ranges intersect. */
static bool ranges_overlap(pointer_range_t left, pointer_range_t right) {
    return left.begin < right.end && right.begin < left.end;
}

/** @brief Verify caller output storage does not alias owner or mapped bytes. */
static bool output_is_disjoint(const interop_mapping_linux_t *owner,
                               const void *output, size_t output_bytes) {
    pointer_range_t output_range;
    pointer_range_t owner_range;
    pointer_range_t mapping_range;
    if (owner == NULL || !pointer_range(output, output_bytes, &output_range) ||
        !pointer_range(owner, sizeof(*owner), &owner_range) ||
        ranges_overlap(output_range, owner_range)) return false;
    if (owner->mapped && pointer_range(owner->mapping_base,
                                      (size_t)owner->mapping_length,
                                      &mapping_range) &&
        ranges_overlap(output_range, mapping_range)) return false;
    return true;
}

/** @brief Enforce the creating pthread without accessing mutable owner state. */
static bool on_owner_thread(const interop_mapping_linux_t *owner) {
    return owner != NULL && pthread_equal(owner->owner_thread, pthread_self()) != 0;
}

/** @brief Duplicate a borrowed descriptor while preserving source flags. */
static int duplicate_cloexec(int borrowed_fd) {
    int duplicated_fd;
    do {
        duplicated_fd = fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0);
    } while (duplicated_fd < 0 && errno == EINTR);
    return duplicated_fd;
}

/** @brief Relinquish a descriptor number before its single Linux close attempt. */
static int release_fd_once(int *fd) {
    int released_fd;
    if (*fd < 0) return 0;
    released_fd = *fd;
    *fd = -1;
    if (close(released_fd) == 0) return 0;
    return errno == 0 ? EIO : errno;
}

/** @brief Check the caller-selected backing-kind facts without reading bytes. */
static interop_mapping_linux_status_t validate_backing(
    int fd, interop_mapping_linux_backing_t backing_kind) {
    struct stat status;
    int descriptor_flags = fcntl(fd, F_GETFL);
    if (descriptor_flags < 0) return InteropMappingLinuxOsError;
    if ((descriptor_flags & O_ACCMODE) != O_RDWR)
        return InteropMappingLinuxBackingRejected;
    if (fstat(fd, &status) != 0) return InteropMappingLinuxOsError;
    if (backing_kind == InteropMappingLinuxBackingRegularPrivateFile) {
        if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
            (uint64_t)status.st_size != InteropMappingLinuxBytes ||
            status.st_uid != geteuid() || (status.st_mode & 07777) != 0600 ||
            status.st_nlink != 1) return InteropMappingLinuxBackingRejected;
        return InteropMappingLinuxOk;
    }
    if (backing_kind == InteropMappingLinuxBackingKvmfrCharacter) {
        long capacity;
        if (!S_ISCHR(status.st_mode)) return InteropMappingLinuxBackingRejected;
        capacity = av_kvmfr_size(fd);
        if (capacity < 0) return InteropMappingLinuxOsError;
        return (uint64_t)capacity == InteropMappingLinuxBytes
            ? InteropMappingLinuxOk : InteropMappingLinuxBackingRejected;
    }
    return InteropMappingLinuxInvalidArgument;
}

interop_mapping_linux_status_t interop_mapping_linux_create(
    interop_mapping_linux_t **owner_out) {
    interop_mapping_linux_t *owner;
    if (owner_out == NULL ||
        (uintptr_t)owner_out % _Alignof(interop_mapping_linux_t *) != 0 ||
        *owner_out != NULL) return InteropMappingLinuxInvalidArgument;
    owner = calloc(1, sizeof(*owner));
    if (owner == NULL) return InteropMappingLinuxOutOfMemory;
    owner->owner_thread = pthread_self();
    owner->owned_fd = -1;
    *owner_out = owner;
    return InteropMappingLinuxOk;
}

interop_mapping_linux_status_t interop_mapping_linux_open(
    interop_mapping_linux_t *owner, int borrowed_fd,
    interop_mapping_linux_backing_t backing_kind) {
    interop_mapping_linux_status_t status;
    int duplicated_fd;
    void *mapping_base;
    if (owner == NULL || borrowed_fd < 0 ||
        (backing_kind != InteropMappingLinuxBackingRegularPrivateFile &&
         backing_kind != InteropMappingLinuxBackingKvmfrCharacter))
        return InteropMappingLinuxInvalidArgument;
    if (!on_owner_thread(owner)) return InteropMappingLinuxWrongThread;
    if (owner->mapped || owner->owned_fd >= 0 || owner->view_published || owner->closing)
        return InteropMappingLinuxBusy;
    duplicated_fd = duplicate_cloexec(borrowed_fd);
    if (duplicated_fd < 0) return InteropMappingLinuxOsError;
    owner->owned_fd = duplicated_fd;
    owner->backing_kind = backing_kind;
    status = validate_backing(duplicated_fd, backing_kind);
    if (status != InteropMappingLinuxOk) {
        int close_error = release_fd_once(&owner->owned_fd);
        if (close_error != 0) {
            errno = close_error;
            return InteropMappingLinuxCloseDiagnostic;
        }
        return status;
    }
    mapping_base = mmap(NULL, (size_t)InteropMappingLinuxBytes,
                        PROT_READ | PROT_WRITE, MAP_SHARED, duplicated_fd, 0);
    if (mapping_base == MAP_FAILED) {
        int close_error = release_fd_once(&owner->owned_fd);
        if (close_error != 0) {
            errno = close_error;
            return InteropMappingLinuxCloseDiagnostic;
        }
        return InteropMappingLinuxOsError;
    }
    owner->mapping_base = mapping_base;
    owner->mapping_length = InteropMappingLinuxBytes;
    owner->mapped = true;
    owner->header_validated = false;
    owner->view_published = false;
    return InteropMappingLinuxOk;
}

interop_mapping_linux_status_t interop_mapping_linux_validate_header(
    interop_mapping_linux_t *owner, uint64_t hello_epoch,
    interop_shm_header_snapshot_t *snapshot) {
    _Alignas(interop_shm_header_t) uint8_t private_header[InteropShmHeaderBytes];
    interop_shm_header_snapshot_t candidate;
    interop_shm_header_t *live_header;
    interop_slot_descriptor_t *first_descriptor;
    interop_layout_status_t layout_status;
    interop_atomic_status_t atomic_status;
    uint64_t guest_heartbeat;
    uint64_t host_heartbeat;
    if (owner == NULL || snapshot == NULL || hello_epoch == 0)
        return InteropMappingLinuxInvalidArgument;
    if (!on_owner_thread(owner)) return InteropMappingLinuxWrongThread;
    if (!owner->mapped || owner->closing || owner->header_validated || owner->view_published)
        return InteropMappingLinuxBusy;
    if (!output_is_disjoint(owner, snapshot, sizeof(*snapshot)) ||
        (uintptr_t)snapshot % _Alignof(interop_shm_header_snapshot_t) != 0)
        return InteropMappingLinuxInvalidArgument;
    live_header = (interop_shm_header_t *)owner->mapping_base;
    first_descriptor = (interop_slot_descriptor_t *)
        ((uint8_t *)owner->mapping_base + UINT64_C(0x1000));
    if (!atomic_is_lock_free(&live_header->guest_heartbeat) ||
        !atomic_is_lock_free(&live_header->host_heartbeat) ||
        !atomic_is_lock_free(&first_descriptor->state))
        return InteropMappingLinuxNotLockFree;
    memcpy(private_header, owner->mapping_base, 56);
    memcpy(private_header + 72, (const uint8_t *)owner->mapping_base + 72,
           InteropShmHeaderBytes - 72);
    guest_heartbeat = atomic_load_explicit(&live_header->guest_heartbeat,
                                           memory_order_acquire);
    host_heartbeat = atomic_load_explicit(&live_header->host_heartbeat,
                                         memory_order_acquire);
    memcpy(private_header + 56, &guest_heartbeat, sizeof(guest_heartbeat));
    memcpy(private_header + 64, &host_heartbeat, sizeof(host_heartbeat));
    layout_status = interop_layout_validate_shm_header(
        private_header, sizeof(private_header), (uintptr_t)owner->mapping_base,
        owner->mapping_length, hello_epoch, &candidate);
    if (layout_status != InteropLayoutStatusOk)
        return InteropMappingLinuxInvalidHeader;
    atomic_status = interop_atomic_validate_mapping(
        (uintptr_t)owner->mapping_base, owner->mapping_length,
        live_header, first_descriptor);
    if (atomic_status == InteropAtomicStatusNotLockFree)
        return InteropMappingLinuxNotLockFree;
    if (atomic_status != InteropAtomicStatusOk)
        return InteropMappingLinuxInvalidHeader;
    *snapshot = candidate;
    owner->header_validated = true;
    return InteropMappingLinuxOk;
}

interop_mapping_linux_status_t interop_mapping_linux_view(
    interop_mapping_linux_t *owner,
    interop_mapping_linux_view_t *view_out) {
    interop_mapping_linux_view_t view;
    if (owner == NULL || view_out == NULL) return InteropMappingLinuxInvalidArgument;
    if (!on_owner_thread(owner)) return InteropMappingLinuxWrongThread;
    if (!owner->mapped || owner->closing || !owner->header_validated || owner->view_published)
        return InteropMappingLinuxBusy;
    if (!output_is_disjoint(owner, view_out, sizeof(*view_out)) ||
        (uintptr_t)view_out % _Alignof(interop_mapping_linux_view_t) != 0)
        return InteropMappingLinuxInvalidArgument;
    view = (interop_mapping_linux_view_t){
        .mapping_base = (uintptr_t)owner->mapping_base,
        .mapping_length = owner->mapping_length,
        .reserved = {0, 0},
    };
    *view_out = view;
    owner->view_published = true;
    return InteropMappingLinuxOk;
}

interop_mapping_linux_status_t interop_mapping_linux_close(
    interop_mapping_linux_t *owner, uint32_t local_users_quiesced) {
    bool cleanup_failed = false;
    if (owner == NULL || local_users_quiesced > 1)
        return InteropMappingLinuxInvalidArgument;
    if (!on_owner_thread(owner)) return InteropMappingLinuxWrongThread;
    if (owner->view_published && local_users_quiesced == 0)
        return InteropMappingLinuxMissingQuiescence;
    owner->closing = true;
    owner->view_published = false;
    owner->header_validated = false;
    if (owner->mapped) {
        if (munmap(owner->mapping_base, (size_t)owner->mapping_length) == 0) {
            owner->mapping_base = NULL;
            owner->mapping_length = 0;
            owner->mapped = false;
        } else {
            cleanup_failed = true;
        }
    }
    if (!owner->mapped && owner->owned_fd >= 0) {
        int close_error = release_fd_once(&owner->owned_fd);
        if (close_error != 0) {
            owner->closing = false;
            errno = close_error;
            return InteropMappingLinuxCloseDiagnostic;
        }
    }
    if (cleanup_failed) return InteropMappingLinuxCleanupPending;
    owner->closing = false;
    return InteropMappingLinuxOk;
}

interop_mapping_linux_status_t interop_mapping_linux_destroy(
    interop_mapping_linux_t **owner_inout) {
    interop_mapping_linux_t *owner;
    if (owner_inout == NULL) return InteropMappingLinuxInvalidArgument;
    if ((uintptr_t)owner_inout % _Alignof(interop_mapping_linux_t *) != 0)
        return InteropMappingLinuxInvalidArgument;
    owner = *owner_inout;
    if (owner == NULL) return InteropMappingLinuxOk;
    if (!on_owner_thread(owner)) return InteropMappingLinuxWrongThread;
    if (!output_is_disjoint(owner, owner_inout, sizeof(*owner_inout)))
        return InteropMappingLinuxInvalidArgument;
    if (owner->mapped || owner->owned_fd >= 0 || owner->view_published || owner->closing)
        return InteropMappingLinuxBusy;
    free(owner);
    *owner_inout = NULL;
    return InteropMappingLinuxOk;
}
