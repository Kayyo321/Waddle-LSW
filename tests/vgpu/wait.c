#include "waddle/venus_wait.h"
#include <assert.h>
#include <string.h>

typedef struct wait_context_t {
    venus_ring_t peer;
    venus_ring_status_t status;
    uint8_t byte;
    uint32_t calls;
    int writing;
} wait_context_t;

static venus_ring_status_t notify(void *argument) {
    wait_context_t *context = argument;
    ++context->calls;
    if (context->status == RingOk) {
        if (context->writing) {
            uint8_t bytes[64];
            assert(venus_ring_read(&context->peer, bytes, sizeof(bytes)) == RingOk);
        } else {
            assert(venus_ring_write(&context->peer, &context->byte, 1) == RingOk);
        }
    }
    return context->status;
}

int main(void) {
    _Alignas(64) uint8_t mapping[VenusRingHeaderBytes + 64];
    venus_ring_t ring;
    uint8_t source[64], result[64];
    memset(source, 0x42, sizeof(source));
    wait_context_t context = {0};
    assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_ring_attach(&ring, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_attach(&context.peer, mapping, sizeof(mapping)) == RingOk);
    assert(venus_ring_write_wait(&ring, source, 1, NULL, NULL) == RingInvalid);
    assert(venus_ring_read_wait(&ring, result, 1, NULL, NULL) == RingInvalid);
    assert(venus_ring_write_wait(NULL, source, 1, notify, &context) == RingInvalid);
    assert(venus_ring_read_wait(NULL, result, 1, notify, &context) == RingInvalid);
    context.writing = 1;
    assert(venus_ring_write(&ring, source, sizeof(source)) == RingOk);
    assert(venus_ring_write_wait(&ring, source, 1, notify, &context) == RingOk);
    assert(context.calls == 1);
    assert(venus_ring_read(&ring, result, 1) == RingOk && result[0] == source[0]);
    context.writing = 0;
    context.byte = 0x73;
    assert(venus_ring_read_wait(&ring, result, 1, notify, &context) == RingOk);
    assert(result[0] == context.byte && context.calls == 2);
    const venus_ring_status_t Outcomes[] = {
        RingCancelled, RingTimeout, RingCorrupt, RingInvalid, RingAgain,
        (venus_ring_status_t)123, RingClosed
    };
    for (size_t index = 0; index < sizeof(Outcomes) / sizeof(Outcomes[0]); ++index) {
        assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
        context.calls = 0;
        context.status = Outcomes[index];
        venus_ring_status_t expected = context.status == RingAgain || context.status == 123 ?
                                       RingInvalid : context.status;
        memset(result, 0xa5, sizeof(result));
        assert(venus_ring_read_wait(&ring, result, 1, notify, &context) == expected);
        assert(result[0] == 0xa5 && context.calls == 1);
        assert(venus_ring_init(mapping, sizeof(mapping), 64) == RingOk);
        assert(venus_ring_write(&ring, source, sizeof(source)) == RingOk);
        uint32_t cursor = atomic_load_explicit(&ring.header->tail, memory_order_acquire);
        assert(venus_ring_write_wait(&ring, source, 1, notify, &context) == expected);
        assert(atomic_load_explicit(&ring.header->tail, memory_order_acquire) == cursor);
    }
    venus_ring_detach(&ring);
    venus_ring_detach(&context.peer);
    return 0;
}
