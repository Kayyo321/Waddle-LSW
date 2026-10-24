/**
 * @file mapping_init.c
 * @brief Sparse-address tests for quiescent guest mapping metadata initialization.
 *
 * Reserves the full frozen virtual range but commits/touches only metadata and
 * one sentinel page. This test does not initialize heap pixels or audio rings.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "interop_mapping_init.h"
#include "waddle/interop_memory.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/** @brief Exact sparse mapping reservation length. */
#define MappingBytes UINT64_C(0x80000000)
/** @brief Metadata range written by the initializer. */
#define MetadataBytes UINT32_C(0x4000)
/** @brief Number of fixed descriptors. */
#define DescriptorCount UINT32_C(192)
/** @brief Sentinel byte outside the metadata range. */
#define SentinelValue UINT8_C(0x5a)
/** @brief Poison byte for unchanged-on-error verification. */
#define PoisonValue UINT8_C(0xa7)

/** @brief Return fixture failure; the caller retains and releases its storage. */
#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "mapping_init.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void *reserve_mapping(void) {
#ifdef _WIN32
    void *mapping = VirtualAlloc(NULL, (SIZE_T)MappingBytes, MEM_RESERVE, PAGE_NOACCESS);
    if (mapping == NULL) {
        return NULL;
    }
    if (VirtualAlloc(mapping, MetadataBytes + 0x1000, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        VirtualFree(mapping, 0, MEM_RELEASE);
        return NULL;
    }
    return mapping;
#else
    void *mapping = mmap(NULL, (size_t)MappingBytes, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    if (mprotect(mapping, MetadataBytes + 0x1000, PROT_READ | PROT_WRITE) != 0) {
        (void)munmap(mapping, (size_t)MappingBytes);
        return NULL;
    }
    return mapping;
#endif
}

static void release_mapping(void *mapping) {
#ifdef _WIN32
    if (mapping != NULL) {
        (void)VirtualFree(mapping, 0, MEM_RELEASE);
    }
#else
    if (mapping != NULL) {
        (void)munmap(mapping, (size_t)MappingBytes);
    }
#endif
}

static int test_invalid_calls_are_unchanged(void *mapping, uint8_t *snapshot) {
    const uintptr_t base = (uintptr_t)mapping;
    uint8_t *sentinel = (uint8_t *)(base + MetadataBytes);

    memset(mapping, PoisonValue, MetadataBytes);
    *sentinel = SentinelValue;
    memcpy(snapshot, mapping, MetadataBytes);
    Check(interop_mapping_initialize(0, MappingBytes, 1, 1) == InteropMappingInitStatusInvalidArgument);
    Check(interop_mapping_initialize(base, MappingBytes - 1, 1, 1) == InteropMappingInitStatusInvalidArgument);
    Check(interop_mapping_initialize(base, MappingBytes, 0, 1) == InteropMappingInitStatusInvalidArgument);
    Check(interop_mapping_initialize(base, MappingBytes, 1, 2) == InteropMappingInitStatusInvalidArgument);
    Check(memcmp(mapping, snapshot, MetadataBytes) == 0);
    Check(interop_mapping_initialize(base + 1, MappingBytes, 1, 1) == InteropMappingInitStatusInvalidArgument);
    Check(memcmp(mapping, snapshot, MetadataBytes) == 0);
    Check(interop_mapping_initialize(UINTPTR_MAX & ~(uintptr_t)63, MappingBytes, 1, 1) == InteropMappingInitStatusInvalidArgument);
    Check(memcmp(mapping, snapshot, MetadataBytes) == 0);
    Check(interop_mapping_initialize(base, MappingBytes, 1, 0) == InteropMappingInitStatusMissingProof);
    Check(memcmp(mapping, snapshot, MetadataBytes) == 0);
    Check(*sentinel == SentinelValue);
    return 0;
}

static int test_initializes_only_metadata(void *mapping) {
    const uintptr_t base = (uintptr_t)mapping;
    interop_shm_header_t *header = mapping;
    interop_slot_descriptor_t *descriptors =
        (interop_slot_descriptor_t *)(base + UINT32_C(0x1000));
    uint8_t *sentinel = (uint8_t *)(base + MetadataBytes);

    memset(mapping, PoisonValue, MetadataBytes);
    *sentinel = SentinelValue;
    Check(interop_mapping_initialize(base, MappingBytes, UINT64_C(0x1122334455667788), 1) == InteropMappingInitStatusOk);
    Check(header->magic == InteropShmMagic);
    Check(header->layout_version == InteropLayoutVersion);
    Check(header->session_epoch == UINT64_C(0x1122334455667788));
    Check(header->total_bytes == MappingBytes);
    Check(header->slot_count == DescriptorCount);
    Check(header->audio_ring_count == 8);
    Check(header->heap_offset == UINT64_C(0x00200000));
    Check(header->heap_bytes == UINT64_C(0x7FC00000));
    Check(header->page_bytes == UINT32_C(0x00200000));
    Check(header->reserved0 == 0);
    Check(atomic_load_explicit(&header->guest_heartbeat, memory_order_relaxed) == 0);
    Check(atomic_load_explicit(&header->host_heartbeat, memory_order_relaxed) == 0);
    for (size_t index = 0; index < sizeof(header->reserved); ++index) {
        Check(header->reserved[index] == 0);
    }
    for (uint32_t index = 0; index < DescriptorCount; ++index) {
        const interop_slot_descriptor_t *descriptor = &descriptors[index];
        Check(atomic_load_explicit(&descriptor->state, memory_order_relaxed) == 0);
        Check(descriptor->format == 0);
        Check(descriptor->window == 0);
        Check(descriptor->generation == 0);
        Check(descriptor->frame_sequence == 0);
        Check(descriptor->data_offset == 0);
        Check(descriptor->data_bytes == 0);
        Check(descriptor->width == 0);
        Check(descriptor->height == 0);
        Check(descriptor->stride == 0);
        Check(descriptor->reserved == 0);
    }
    Check(*sentinel == SentinelValue);
    return 0;
}

int main(void) {
    void *mapping = reserve_mapping();
    uint8_t *snapshot = malloc(MetadataBytes);
    int result = 0;

    if (mapping == NULL || snapshot == NULL) {
        fprintf(stderr, "unable to reserve sparse mapping fixture\n");
        result = 1;
        goto cleanup;
    }
    if (test_invalid_calls_are_unchanged(mapping, snapshot) != 0 ||
        test_initializes_only_metadata(mapping) != 0) {
        result = 1;
        goto cleanup;
    }

cleanup:
    free(snapshot);
    release_mapping(mapping);
    if (result == 0) {
        puts("mapping metadata initialization tests passed");
    }
    return result;
}
