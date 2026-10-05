#include "waddle/venus_ring.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

static void test_arguments(void) {
    _Alignas(64) uint8_t mapping[VenusRingHeaderBytes + 128];
    uint8_t before[sizeof(mapping)];
    venus_ring_t ring = {0};
    uint8_t byte = 0xa5;
    memset(mapping, 0xa5, sizeof(mapping));
    memcpy(before, mapping, sizeof(before));
    assert(venus_ring_init(NULL, sizeof(mapping), 64) == RingInvalid);
    assert(venus_ring_init(mapping + 1, sizeof(mapping) - 1, 64) == RingInvalid);
    assert(venus_ring_init(mapping, 191, 64) == RingInvalid);
    assert(venus_ring_init(mapping, sizeof(mapping), 0) == RingInvalid);
    assert(venus_ring_init(mapping, sizeof(mapping), 32) == RingInvalid);
    assert(venus_ring_init(mapping, sizeof(mapping), 65) == RingInvalid);
    assert(venus_ring_init(mapping, sizeof(mapping), VenusRingMaxCapacity + 1) == RingInvalid);
    assert(venus_ring_init(mapping, 255, 64) == RingInvalid);
    assert(memcmp(before, mapping, sizeof(mapping)) == 0);
    assert(venus_ring_attach(NULL, mapping, sizeof(mapping)) == RingInvalid);
    assert(venus_ring_attach(&ring, NULL, sizeof(mapping)) == RingInvalid);
    assert(venus_ring_attach(&ring, mapping + 1, sizeof(mapping) - 1) == RingInvalid);
    assert(venus_ring_attach(&ring, mapping, 191) == RingInvalid);
    assert(venus_ring_attach(&ring, mapping, sizeof(mapping)) == RingInvalid);
    assert(venus_ring_write(NULL, &byte, 1) == RingInvalid);
    assert(venus_ring_read(NULL, &byte, 1) == RingInvalid);
    assert(venus_ring_close(NULL) == RingInvalid);
    assert(venus_ring_close(&ring) == RingInvalid);
    assert(venus_ring_write(&ring, &byte, 1) == RingInvalid);
    assert(venus_ring_read(&ring, &byte, 1) == RingInvalid);
    assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_ring_attach(&ring, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_write(&ring, NULL, 1) == RingInvalid);
    assert(venus_ring_read(&ring, NULL, 1) == RingInvalid);
    assert(venus_ring_write(&ring, &byte, 0) == RingInvalid);
    assert(venus_ring_read(&ring, &byte, 0) == RingInvalid);
    assert(venus_ring_write(&ring, &byte, 65) == RingInvalid);
    assert(venus_ring_read(&ring, &byte, 65) == RingInvalid);
    ring.payload = NULL;
    assert(venus_ring_close(&ring) == RingInvalid);
    ring.payload = mapping + VenusRingHeaderBytes;
    ring.capacity = 65;
    assert(venus_ring_close(&ring) == RingInvalid);
    venus_ring_detach(NULL);
    venus_ring_detach(&ring);
    assert(ring.header == NULL && ring.payload == NULL && ring.capacity == 0);
}

static void test_corruption(void) {
    _Alignas(64) uint8_t mapping[VenusRingHeaderBytes + 64];
    venus_ring_t ring, second;
    uint8_t result = 0xa5, source = 0x42;
    assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_ring_attach(&ring, mapping, sizeof(mapping)) == RingOk);
    for (size_t offset = 0; offset < VenusRingHeaderBytes; ++offset) {
        if ((offset >= 16 && offset < 20) || (offset >= 64 && offset < 68) ||
            (offset >= 128 && offset < 132))
            continue;
        mapping[offset] ^= 0x80;
        assert(venus_ring_attach(&second, mapping, sizeof(mapping)) == RingInvalid);
        assert(second.header == NULL && second.payload == NULL && second.capacity == 0);
        mapping[offset] ^= 0x80;
    }
    assert(venus_ring_attach(&second, mapping, 255) == RingInvalid);
    atomic_store_explicit(&ring.header->flags, 2, memory_order_release);
    assert(venus_ring_attach(&second, mapping, sizeof(mapping)) == RingCorrupt);
    assert(venus_ring_write(&ring, &source, 1) == RingCorrupt);
    assert(venus_ring_read(&ring, &result, 1) == RingCorrupt);
    atomic_store_explicit(&ring.header->flags, 0, memory_order_release);
    atomic_store_explicit(&ring.header->tail, 65, memory_order_release);
    assert(venus_ring_write(&ring, &source, 1) == RingCorrupt);
    assert(venus_ring_read(&ring, &result, 1) == RingCorrupt);
    assert(result == 0xa5);
    atomic_store_explicit(&ring.header->tail, 0, memory_order_release);
    ring.header->capacity = UINT32_MAX;
    ring.header->header_bytes = UINT32_MAX;
    assert(venus_ring_write(&ring, &source, 1) == RingOk);
    assert(venus_ring_read(&ring, &result, 1) == RingOk);
    assert(result == source); /* Local capacity survives peer metadata changes. */
    ring.header->capacity = 64;
    ring.header->header_bytes = VenusRingHeaderBytes;
    assert(venus_ring_close(&ring) == RingOk);
    assert(venus_ring_attach(&second, mapping, sizeof(mapping)) == RingClosed);
    assert(second.header == NULL);
}

static void test_stream(void) {
    _Alignas(64) uint8_t mapping[VenusRingHeaderBytes + 128];
    venus_ring_t producer, consumer;
    uint8_t source[64], result[64];
    for (size_t index = 0; index < sizeof(source); ++index)
        source[index] = (uint8_t)index;
    memset(mapping, 0xa5, sizeof(mapping));
    assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(mapping[VenusRingHeaderBytes] == 0xa5);
    assert(venus_ring_attach(&producer, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_attach(&consumer, mapping, sizeof(mapping)) == RingOk);
    for (uint32_t cursor = 0; cursor < 64; ++cursor) {
        atomic_store_explicit(&producer.header->head, UINT32_MAX - cursor, memory_order_release);
        atomic_store_explicit(&producer.header->tail, UINT32_MAX - cursor, memory_order_release);
        for (size_t length = 1; length <= sizeof(source); ++length) {
            memset(result, 0xa5, sizeof(result));
            assert(venus_ring_read(&consumer, result, length) == RingAgain);
            assert(result[0] == 0xa5);
            assert(venus_ring_write(&producer, source, length) == RingOk);
            assert(venus_ring_read(&consumer, result, length) == RingOk);
            assert(memcmp(source, result, length) == 0);
        }
    }
    assert(venus_ring_write(&producer, source, 64) == RingOk);
    uint32_t tail = atomic_load_explicit(&producer.header->tail, memory_order_acquire);
    assert(venus_ring_write(&producer, source, 1) == RingAgain);
    assert(atomic_load_explicit(&producer.header->tail, memory_order_acquire) == tail);
    assert(venus_ring_read(&consumer, result, 64) == RingOk);
    assert(memcmp(source, result, sizeof(source)) == 0);
    for (size_t index = VenusRingHeaderBytes + 64; index < sizeof(mapping); ++index)
        assert(mapping[index] == 0xa5);
    assert(venus_ring_close(&producer) == RingOk);
    assert(venus_ring_close(&producer) == RingOk);
    assert(venus_ring_read(&consumer, result, 1) == RingClosed);
    assert(venus_ring_write(&producer, source, 1) == RingClosed);
    venus_ring_detach(&producer);
    venus_ring_detach(&consumer);
}

static void test_max_capacity(void) {
    size_t extent = VenusRingHeaderBytes + VenusRingMaxCapacity;
#ifdef _WIN32
    uint8_t *mapping = _aligned_malloc(extent, 64);
#else
    uint8_t *mapping = aligned_alloc(64, extent);
#endif
    assert(mapping);
    venus_ring_t ring;
    assert(venus_ring_init(mapping, extent, VenusRingMaxCapacity) == RingOk);
    assert(venus_ring_attach(&ring, mapping, extent) == RingOk);
    venus_ring_detach(&ring);
#ifdef _WIN32
    _aligned_free(mapping);
#else
    free(mapping);
#endif
    mapping = NULL;
}

int main(void) {
    test_arguments();
    test_corruption();
    test_stream();
    test_max_capacity();
    return 0;
}
