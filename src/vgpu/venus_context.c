/** @file venus_context.c @brief Independent process contexts and aggregate ownership. */
#include "waddle/venus_context.h"
#include "waddle/venus_region.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static venus_context_slot_t *find_slot(venus_context_manager_t *manager, uint64_t identity) {
    if (!manager || !manager->count_limit || !identity)
        return NULL;
    for (uint32_t index = 0; index < VenusContextMaxCount; index++)
        if (manager->slots[index].identity == identity)
            return &manager->slots[index];
    return NULL;
}

/* Region parsing uses the bounded Zig adapter. No shared extent is trusted. */
static venus_ring_status_t inspect_mapping(int fd, uint64_t bytes, uint32_t *capacity) {
    void *mapping = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return RingCorrupt;
    venus_region_view_t region = {0};
    venus_ring_status_t result = venus_region_attach(&region, mapping, (size_t)bytes);
    if (result == RingOk) {
        if (atomic_load_explicit(&region.commands.header->head, memory_order_acquire) ||
            atomic_load_explicit(&region.commands.header->tail, memory_order_acquire) ||
            atomic_load_explicit(&region.commands.header->flags, memory_order_acquire) ||
            atomic_load_explicit(&region.replies.header->head, memory_order_acquire) ||
            atomic_load_explicit(&region.replies.header->tail, memory_order_acquire) ||
            atomic_load_explicit(&region.replies.header->flags, memory_order_acquire))
            result = RingInvalid;
        else
            *capacity = region.commands.capacity;
    }
    venus_region_detach(&region);
    munmap(mapping, (size_t)bytes);
    return result == RingOk ? RingOk : RingInvalid;
}

static venus_ring_status_t release_slot(venus_context_manager_t *manager,
                                        venus_context_slot_t *slot) {
    venus_ring_status_t result = RingCorrupt;
    void *mapping =
        mmap(NULL, (size_t)slot->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, slot->mapping_fd, 0);
    if (mapping != MAP_FAILED) {
        unsigned char *base = mapping;
        venus_ring_t commands = {.header = (void *)(base + VenusRegionHeaderBytes),
                                 .payload = base + VenusRegionHeaderBytes + VenusRingHeaderBytes,
                                 .capacity = slot->capacity};
        venus_ring_t replies = {
            .header =
                (void *)(base + VenusRegionHeaderBytes + VenusRingHeaderBytes + slot->capacity),
            .payload = base + VenusRegionHeaderBytes + 2 * VenusRingHeaderBytes + slot->capacity,
            .capacity = slot->capacity};
        /* Close only the validated creation-time offsets, even if a peer
         * corrupts or closes immutable metadata before teardown. */
        venus_ring_close(&commands);
        venus_ring_close(&replies);
        munmap(mapping, (size_t)slot->bytes);
        result = RingOk;
    }
    close(slot->mapping_fd);
    manager->live_bytes -= slot->bytes;
    manager->live_count--;
    memset(slot, 0, sizeof(*slot));
    return result == RingOk ? RingOk : RingCorrupt;
}

venus_ring_status_t venus_context_manager_init(venus_context_manager_t *manager, uint32_t count,
                                               uint64_t bytes) {
    if (!manager || manager->count_limit || !count || count > VenusContextMaxCount ||
        bytes < 4096 || bytes > UINT64_C(8589934592))
        return RingInvalid;
    *manager =
        (venus_context_manager_t){.next_identity = 1, .byte_limit = bytes, .count_limit = count};
    return RingOk;
}

venus_ring_status_t venus_context_create(venus_context_manager_t *manager, const char *path,
                                         int mapping_fd, int stream_fd, uint64_t *identity) {
    if (!identity)
        return RingInvalid;
    *identity = 0;
    struct stat metadata;
    if (!manager || !manager->count_limit || !path || path[0] != '/' ||
        fstat(mapping_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 4096 || metadata.st_size > 1073741824 ||
        ((uint64_t)metadata.st_size & ((uint64_t)metadata.st_size - 1)))
        return RingInvalid;
    if (!manager->next_identity || manager->live_count == manager->count_limit ||
        (uint64_t)metadata.st_size > manager->byte_limit - manager->live_bytes)
        return RingLimit;
    venus_context_slot_t *available = NULL;
    for (uint32_t index = 0; index < VenusContextMaxCount; index++) {
        venus_context_slot_t *slot = &manager->slots[index];
        if (slot->identity) {
            if (slot->device == metadata.st_dev && slot->inode == metadata.st_ino)
                return RingInvalid;
        } else if (!available)
            available = slot;
    }
    uint32_t capacity = 0;
    venus_ring_status_t result = inspect_mapping(mapping_fd, (uint64_t)metadata.st_size, &capacity);
    if (result != RingOk)
        return result;
    int owned_fd = fcntl(mapping_fd, F_DUPFD_CLOEXEC, 5);
    if (owned_fd < 0)
        return RingCorrupt;
    venus_worker_t worker = {0};
    result = venus_worker_create(&worker, path, owned_fd, stream_fd);
    if (result != RingOk) {
        close(owned_fd);
        return result;
    }
    *available = (venus_context_slot_t){.worker = worker,
                                        .identity = manager->next_identity,
                                        .bytes = (uint64_t)metadata.st_size,
                                        .capacity = capacity,
                                        .device = metadata.st_dev,
                                        .inode = metadata.st_ino,
                                        .mapping_fd = owned_fd};
    *identity = manager->next_identity++;
    manager->live_count++;
    manager->live_bytes += available->bytes;
    return RingOk;
}

venus_ring_status_t venus_context_poll(venus_context_manager_t *manager, uint64_t identity) {
    venus_context_slot_t *slot = find_slot(manager, identity);
    if (!slot)
        return RingInvalid;
    venus_ring_status_t result = venus_worker_poll(&slot->worker);
    if (result != RingClosed)
        return result;
    return release_slot(manager, slot) == RingOk ? RingClosed : RingCorrupt;
}

venus_ring_status_t venus_context_destroy(venus_context_manager_t *manager, uint64_t identity,
                                          uint32_t timeout_ms) {
    venus_context_slot_t *slot = find_slot(manager, identity);
    if (!slot || !timeout_ms || timeout_ms > 60000)
        return RingInvalid;
    venus_ring_status_t result = venus_worker_destroy(&slot->worker, timeout_ms);
    return result == RingOk ? release_slot(manager, slot) : result;
}

venus_ring_status_t venus_context_manager_free(venus_context_manager_t *manager,
                                               uint32_t timeout_ms) {
    if (!manager || !manager->count_limit)
        return RingOk;
    if (!timeout_ms || timeout_ms > 60000)
        return RingInvalid;
    venus_ring_status_t first_error = RingOk;
    for (uint32_t index = 0; index < VenusContextMaxCount; index++) {
        uint64_t identity = manager->slots[index].identity;
        if (!identity)
            continue;
        venus_ring_status_t result = venus_context_destroy(manager, identity, timeout_ms);
        if (first_error == RingOk && result != RingOk)
            first_error = result;
    }
    if (!manager->live_count)
        memset(manager, 0, sizeof(*manager));
    return first_error;
}
