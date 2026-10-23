#include "waddle/venus_region.h"
#include <assert.h>
#include <string.h>

int main(void) {
    _Alignas(64) uint8_t mapping[4096];
    uint8_t saved[sizeof(mapping)];
    venus_region_view_t host, guest;
    uint8_t byte = 0x42, received = 0;
    memset(mapping, 0xa5, sizeof(mapping));
    memcpy(saved, mapping, sizeof(saved));
    assert(venus_region_init(NULL, sizeof(mapping), 64) == RingInvalid);
    assert(venus_region_init(mapping + 1, sizeof(mapping) - 1, 64) == RingInvalid);
    assert(venus_region_init(mapping, 4095, 64) == RingInvalid);
    assert(venus_region_init(mapping, sizeof(mapping), 4096) == RingInvalid);
    assert(venus_region_init(mapping, sizeof(mapping), 0) == RingInvalid);
    assert(memcmp(mapping, saved, sizeof(mapping)) == 0);
    assert(venus_region_attach(NULL, mapping, sizeof(mapping)) == RingInvalid);
    assert(venus_region_attach(&host, NULL, sizeof(mapping)) == RingInvalid);
    assert(venus_region_attach(&host, mapping + 1, sizeof(mapping) - 1) == RingInvalid);
    assert(venus_region_attach(&host, mapping, sizeof(mapping)) == RingInvalid);
    assert(venus_region_init(mapping, sizeof(mapping), 1024) == RingOk);
    assert(venus_region_attach(&host, mapping, sizeof(mapping)) == RingOk);
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingOk);
    assert(host.resources == mapping + 64 + 2 * (192 + 1024));
    assert(host.resource_bytes == sizeof(mapping) - (size_t)(host.resources - mapping));
    assert(host.resources[0] == 0xa5);
    for (size_t offset = 0; offset < 64; ++offset) {
        mapping[offset] ^= 0x80;
        assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingInvalid);
        assert(guest.commands.header == NULL && guest.replies.header == NULL);
        mapping[offset] ^= 0x80;
    }
    assert(venus_region_attach(&guest, mapping, 63) == RingInvalid);
    assert(venus_region_attach(&guest, mapping, sizeof(mapping) - 1) == RingInvalid);
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_write(&guest.commands, &byte, 1) == RingOk);
    assert(venus_ring_read(&host.commands, &received, 1) == RingOk && received == byte);
    ++byte;
    assert(venus_ring_write(&host.replies, &byte, 1) == RingOk);
    assert(venus_ring_read(&guest.replies, &received, 1) == RingOk && received == byte);
    host.commands.header->magic ^= 1;
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingInvalid);
    host.commands.header->magic ^= 1;
    host.replies.header->magic ^= 1;
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingInvalid);
    host.replies.header->magic ^= 1;
    host.commands.header->capacity = 64;
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingInvalid);
    host.commands.header->capacity = 1024;
    host.replies.header->capacity = 64;
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingInvalid);
    host.replies.header->capacity = 1024;
    assert(venus_ring_close(&host.commands) == RingOk);
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingClosed);
    assert(venus_region_init(mapping, sizeof(mapping), 1024) == RingOk);
    assert(venus_ring_close(&host.replies) == RingOk);
    assert(venus_region_attach(&guest, mapping, sizeof(mapping)) == RingClosed);
    venus_region_detach(&host);
    venus_region_detach(&guest);
    venus_region_detach(NULL);
    assert(host.resources == NULL && host.resource_bytes == 0);
    return 0;
}
