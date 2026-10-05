#include "waddle/venus_region.h"
#include "venus_bounds.h"
#include <string.h>

venus_ring_status_t venus_region_init(void *mapping, size_t mapping_bytes, uint32_t capacity) {
    if (!mapping || ((uintptr_t)mapping & 63u) ||
        !venus_bounds_region_size(mapping_bytes, capacity)) return RingInvalid;
    uint32_t reply_offset = VenusRegionHeaderBytes + VenusRingHeaderBytes + capacity;
    uint32_t resource_offset = reply_offset + VenusRingHeaderBytes + capacity;
    venus_region_header_t *header = mapping;
    memset(header, 0, sizeof(*header));
    header->magic = VenusRegionMagic;
    header->version = VenusRingVersion;
    header->mapping_bytes = mapping_bytes;
    header->capacity = capacity;
    header->command_offset = VenusRegionHeaderBytes;
    header->reply_offset = reply_offset;
    header->resource_offset = resource_offset;
    /* Zig validation proved these offsets and extents before the first write. */
    venus_ring_status_t status = venus_ring_init((uint8_t *)mapping + VenusRegionHeaderBytes,
                                                VenusRingHeaderBytes + capacity, capacity);
    if (status != RingOk) return status;
    return venus_ring_init((uint8_t *)mapping + reply_offset,
                           VenusRingHeaderBytes + capacity, capacity);
}

venus_ring_status_t venus_region_attach(venus_region_view_t *view, void *mapping,
                                       size_t mapping_bytes) {
    if (!view) return RingInvalid;
    memset(view, 0, sizeof(*view));
    if (!mapping || ((uintptr_t)mapping & 63u)) return RingInvalid;
    uint64_t region_bytes = 0;
    uint32_t capacity = venus_bounds_region_capacity(mapping, mapping_bytes, &region_bytes);
    if (!capacity) return RingInvalid;
    uint32_t reply_offset = VenusRegionHeaderBytes + VenusRingHeaderBytes + capacity;
    uint32_t resource_offset = reply_offset + VenusRingHeaderBytes + capacity;
    venus_region_view_t attached = {0};
    venus_ring_status_t status = venus_ring_attach(&attached.commands,
        (uint8_t *)mapping + VenusRegionHeaderBytes, VenusRingHeaderBytes + capacity);
    if (status != RingOk) return status;
    if (attached.commands.capacity != capacity) return RingInvalid;
    status = venus_ring_attach(&attached.replies, (uint8_t *)mapping + reply_offset,
                               VenusRingHeaderBytes + capacity);
    if (status != RingOk) return status;
    if (attached.replies.capacity != capacity) return RingInvalid;
    attached.resources = (uint8_t *)mapping + resource_offset;
    attached.resource_bytes = (size_t)region_bytes - resource_offset;
    *view = attached;
    return RingOk;
}

void venus_region_detach(venus_region_view_t *view) {
    if (view) memset(view, 0, sizeof(*view));
}
