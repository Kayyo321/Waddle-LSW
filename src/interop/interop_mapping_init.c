/**
 * @file interop_mapping_init.c
 * @brief Quiescent initialization of frozen layout-v3 guest metadata.
 *
 * No heap, audio ring, operating-system mapping, peer synchronization, or
 * ownership lifetime is managed here. See interop_mapping_init.h.
 */
#include "interop_mapping_init.h"

#include "waddle/interop_memory.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if !defined(__x86_64__) && !defined(_M_X64)
#error "Interop mapping initialization requires x86_64"
#endif

/** @brief Exact mapping length in bytes. */
#define InteropMappingBytes UINT64_C(0x80000000)
/** @brief Exact written metadata prefix in bytes. */
#define InteropMetadataBytes UINT32_C(0x4000)
/** @brief Exact descriptor-table offset from mapping base. */
#define InteropDescriptorOffset UINT32_C(0x1000)
/** @brief Frozen descriptors initialized by one call. */
#define InteropDescriptorCount UINT32_C(192)
/** @brief Frozen heap offset written in the header. */
#define InteropHeapOffset UINT64_C(0x00200000)
/** @brief Frozen heap length written in the header. */
#define InteropHeapBytes UINT64_C(0x7FC00000)
/** @brief Frozen heap allocation page size. */
#define InteropPageBytes UINT32_C(0x00200000)
/** @brief Frozen audio ring count written in the header. */
#define InteropAudioRingCount UINT32_C(8)
/** @brief Frozen Free ownership state written to each descriptor. */
#define InteropMappingSlotFree UINT32_C(0)

_Static_assert(sizeof(uint32_t) == 4, "mapping state width is frozen at 32 bits");
_Static_assert(sizeof(uint64_t) == 8, "mapping heartbeat width is frozen at 64 bits");
_Static_assert(sizeof(_Atomic uint32_t) == 4, "atomic descriptor state retains frozen width");
_Static_assert(sizeof(_Atomic uint64_t) == 8, "atomic heartbeat retains frozen width");
_Static_assert(_Alignof(interop_shm_header_t) == 64, "header alignment is frozen");
_Static_assert(_Alignof(interop_slot_descriptor_t) == 64, "descriptor alignment is frozen");
_Static_assert(sizeof(interop_shm_header_t) == 0x1000, "header footprint is frozen");
_Static_assert(sizeof(interop_slot_descriptor_t) == 64, "descriptor footprint is frozen");
_Static_assert(InteropDescriptorOffset + InteropDescriptorCount * sizeof(interop_slot_descriptor_t) == InteropMetadataBytes,
               "metadata prefix exactly covers header and descriptors");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "x86_64 int atomics must always be lock-free");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2, "x86_64 long atomics must always be lock-free");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "x86_64 long long atomics must always be lock-free");

/**
 * @brief Check actual local atomic objects before modifying shared storage.
 * @return True when both required atomic widths are lock-free on this process.
 */
static bool local_atomics_are_lock_free(void) {
    _Alignas(8) _Atomic uint32_t state_probe;
    _Alignas(8) _Atomic uint64_t heartbeat_probe;

    atomic_init(&state_probe, 0);
    atomic_init(&heartbeat_probe, 0);
    return atomic_is_lock_free(&state_probe) && atomic_is_lock_free(&heartbeat_probe);
}

interop_mapping_init_status_t interop_mapping_initialize(
    uintptr_t mapping_base,
    uint64_t mapping_length,
    uint64_t session_epoch,
    uint32_t exclusive_owner_proof) {
    interop_shm_header_t *header;
    interop_slot_descriptor_t *descriptors;

    if (mapping_base == 0 || mapping_length != InteropMappingBytes ||
        mapping_base % InteropShmAlignment != 0 ||
        mapping_length > (uint64_t)UINTPTR_MAX ||
        mapping_base > UINTPTR_MAX - (uintptr_t)mapping_length ||
        session_epoch == 0 || exclusive_owner_proof > 1) {
        return InteropMappingInitStatusInvalidArgument;
    }
    if (exclusive_owner_proof == 0) {
        return InteropMappingInitStatusMissingProof;
    }
    if (!local_atomics_are_lock_free()) {
        return InteropMappingInitStatusNotLockFree;
    }

    header = (interop_shm_header_t *)mapping_base;
    descriptors = (interop_slot_descriptor_t *)(mapping_base + InteropDescriptorOffset);

    memset((void *)mapping_base, 0, InteropMetadataBytes);

    header->magic = InteropShmMagic;
    header->layout_version = InteropLayoutVersion;
    header->session_epoch = session_epoch;
    header->total_bytes = InteropMappingBytes;
    header->slot_count = InteropDescriptorCount;
    header->audio_ring_count = InteropAudioRingCount;
    header->heap_offset = InteropHeapOffset;
    header->heap_bytes = InteropHeapBytes;
    header->page_bytes = InteropPageBytes;
    atomic_init(&header->guest_heartbeat, 0);
    atomic_init(&header->host_heartbeat, 0);

    for (uint32_t index = 0; index < InteropDescriptorCount; ++index) {
        atomic_init(&descriptors[index].state, InteropMappingSlotFree);
    }

    return InteropMappingInitStatusOk;
}
