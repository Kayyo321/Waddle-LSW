#include "waddle/venus_ring.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define TransferCount 100000u
#define ChunkBytes 31u
#define MappingBytes (VenusRingHeaderBytes + 1024u)

typedef struct stress_context_t {
    venus_ring_t ring;
    int producer;
} stress_context_t;

static void *transfer(void *argument) {
    stress_context_t *context = argument;
    uint8_t bytes[ChunkBytes];
    for (uint32_t sequence = 0; sequence < TransferCount; ++sequence) {
        if (context->producer)
            for (uint32_t index = 0; index < ChunkBytes; ++index)
                bytes[index] = (uint8_t)(sequence * ChunkBytes + index);
        venus_ring_status_t status;
        do {
            status = context->producer ? venus_ring_write(&context->ring, bytes, sizeof(bytes))
                                       : venus_ring_read(&context->ring, bytes, sizeof(bytes));
            if (status == RingAgain)
                sched_yield();
        } while (status == RingAgain);
        assert(status == RingOk);
        if (!context->producer)
            for (uint32_t index = 0; index < ChunkBytes; ++index)
                assert(bytes[index] == (uint8_t)(sequence * ChunkBytes + index));
    }
    return NULL;
}

static void attach_contexts(void *mapping, stress_context_t *producer, stress_context_t *consumer) {
    assert(venus_ring_init(mapping, MappingBytes, 1024) == RingOk);
    assert(venus_ring_attach(&producer->ring, mapping, MappingBytes) == RingOk);
    assert(venus_ring_attach(&consumer->ring, mapping, MappingBytes) == RingOk);
    producer->producer = 1;
    consumer->producer = 0;
    /* Begin near rollover to exercise modulo arithmetic while concurrent. */
    atomic_store_explicit(&producer->ring.header->tail, UINT32_MAX - 512, memory_order_release);
    atomic_store_explicit(&producer->ring.header->head, UINT32_MAX - 512, memory_order_release);
}

int main(void) {
    alarm(30); /* A deadlocked test must fail rather than hang CI indefinitely. */
    void *mapping =
        mmap(NULL, MappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    stress_context_t producer, consumer;
    attach_contexts(mapping, &producer, &consumer);
    pthread_t producer_thread, consumer_thread;
    assert(pthread_create(&producer_thread, NULL, transfer, &producer) == 0);
    assert(pthread_create(&consumer_thread, NULL, transfer, &consumer) == 0);
    assert(pthread_join(producer_thread, NULL) == 0);
    assert(pthread_join(consumer_thread, NULL) == 0);
    venus_ring_detach(&producer.ring);
    venus_ring_detach(&consumer.ring);
    attach_contexts(mapping, &producer, &consumer);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(30);
        transfer(&producer);
        venus_ring_detach(&producer.ring);
        venus_ring_detach(&consumer.ring);
        assert(munmap(mapping, MappingBytes) == 0);
        return 0;
    }
    transfer(&consumer);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(venus_ring_close(&consumer.ring) == RingOk);
    venus_ring_detach(&producer.ring);
    venus_ring_detach(&consumer.ring);
    assert(munmap(mapping, MappingBytes) == 0);
    mapping = NULL;
    alarm(0);
    return 0;
}
