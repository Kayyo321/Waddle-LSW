#include "waddle/venus_ring.h"
#include "venus_bounds.h"
#include <string.h>

static int valid_capacity(uint32_t capacity) {
    return capacity >= VenusRingMinCapacity && capacity <= VenusRingMaxCapacity &&
           (capacity & (capacity - 1u)) == 0;
}

static int valid_mapping(const void *mapping, size_t mapping_bytes) {
    return mapping && ((uintptr_t)mapping & 63u) == 0 && mapping_bytes >= VenusRingHeaderBytes;
}

static int valid_ring(const venus_ring_t *ring) {
    return ring && ring->header && ring->payload && valid_capacity(ring->capacity);
}

static venus_ring_status_t check_flags(const venus_ring_t *ring) {
    uint32_t flags = atomic_load_explicit(&ring->header->flags, memory_order_acquire);
    if (flags & ~VenusRingClosed)
        return RingCorrupt;
    return flags & VenusRingClosed ? RingClosed : RingOk;
}

venus_ring_status_t venus_ring_init(void *mapping, size_t mapping_bytes, uint32_t capacity) {
    if (!valid_mapping(mapping, mapping_bytes) || !valid_capacity(capacity) ||
        capacity > mapping_bytes - VenusRingHeaderBytes)
        return RingInvalid;
    venus_ring_header_t *header = mapping;
    memset(header, 0, sizeof(*header));
    header->magic = VenusRingMagic;
    header->version = VenusRingVersion;
    header->capacity = capacity;
    header->header_bytes = VenusRingHeaderBytes;
    atomic_init(&header->flags, 0);
    atomic_init(&header->tail, 0);
    atomic_init(&header->head, 0);
    return RingOk;
}

venus_ring_status_t venus_ring_attach(venus_ring_t *ring, void *mapping, size_t mapping_bytes) {
    if (!ring)
        return RingInvalid;
    memset(ring, 0, sizeof(*ring));
    if (!valid_mapping(mapping, mapping_bytes))
        return RingInvalid;
    uint32_t capacity = venus_bounds_capacity(mapping, mapping_bytes);
    if (!capacity)
        return RingInvalid;
    venus_ring_t attached = {mapping, (uint8_t *)mapping + VenusRingHeaderBytes, capacity};
    venus_ring_status_t status = check_flags(&attached);
    if (status == RingOk)
        *ring = attached;
    return status;
}

venus_ring_status_t venus_ring_write(venus_ring_t *ring, const void *data, size_t length) {
    if (!valid_ring(ring) || !data || !length || length > ring->capacity)
        return RingInvalid;
    venus_ring_status_t status = check_flags(ring);
    if (status != RingOk)
        return status;
    uint32_t tail = atomic_load_explicit(&ring->header->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&ring->header->head, memory_order_acquire);
    uint32_t used = tail - head;
    if (used > ring->capacity)
        return RingCorrupt;
    if (length > ring->capacity - used)
        return RingAgain;
    venus_bounds_write(ring->payload, ring->capacity, tail, data, length);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&ring->header->tail, tail + (uint32_t)length, memory_order_release);
    return RingOk;
}

venus_ring_status_t venus_ring_read(venus_ring_t *ring, void *data, size_t length) {
    if (!valid_ring(ring) || !data || !length || length > ring->capacity)
        return RingInvalid;
    venus_ring_status_t status = check_flags(ring);
    if (status != RingOk)
        return status;
    uint32_t head = atomic_load_explicit(&ring->header->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ring->header->tail, memory_order_acquire);
    uint32_t used = tail - head;
    if (used > ring->capacity)
        return RingCorrupt;
    if (length > used)
        return RingAgain;
    atomic_thread_fence(memory_order_acquire);
    venus_bounds_read(ring->payload, ring->capacity, head, data, length);
    atomic_store_explicit(&ring->header->head, head + (uint32_t)length, memory_order_release);
    return RingOk;
}

venus_ring_status_t venus_ring_close(venus_ring_t *ring) {
    if (!valid_ring(ring))
        return RingInvalid;
    atomic_fetch_or_explicit(&ring->header->flags, VenusRingClosed, memory_order_acq_rel);
    return RingOk;
}

void venus_ring_detach(venus_ring_t *ring) {
    if (ring)
        memset(ring, 0, sizeof(*ring));
}
