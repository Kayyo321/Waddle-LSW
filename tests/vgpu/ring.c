#include "waddle/venus_ring.h"
#include <assert.h>
#include <string.h>

int main(void) {
    _Alignas(64) uint8_t mapping[VenusRingHeaderBytes + 64];
    venus_ring_t producer, consumer;
    uint8_t source[64], result[64];
    for (size_t index = 0; index < sizeof(source); ++index) source[index] = (uint8_t)index;
    memset(mapping, 0xa5, sizeof(mapping));
    assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(mapping[VenusRingHeaderBytes] == 0xa5);
    assert(venus_ring_attach(&producer, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_attach(&consumer, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_read(&consumer, result, 64) == RingAgain);
    assert(venus_ring_write(&producer, source, 64) == RingOk);
    assert(venus_ring_write(&producer, source, 1) == RingAgain);
    assert(venus_ring_read(&consumer, result, 64) == RingOk);
    assert(memcmp(source, result, sizeof(source)) == 0);
    assert(venus_ring_close(&producer) == RingOk);
    assert(venus_ring_close(&producer) == RingOk);
    assert(venus_ring_read(&consumer, result, 1) == RingClosed);
    assert(venus_ring_write(&producer, source, 1) == RingClosed);
    venus_ring_detach(&producer);
    venus_ring_detach(&consumer);
    assert(producer.header == NULL && consumer.payload == NULL);
    return 0;
}
