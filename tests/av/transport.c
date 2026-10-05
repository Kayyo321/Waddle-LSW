#include "av_dmabuf.h"
#include "av_layout.h"
#include "av_transport.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
/** @brief Stress duration in monotonically ordered frames, no dynamic ownership. */
enum { StressFrames = 100000, RingFrames = 256 };
static window_slot_header_t slots[3];
static uint64_t pixel_sequences[3];
static audio_ring_header_t ring;
static uint32_t pcm[RingFrames];
static void *produce(void *context) {
    (void)context;
    for (uint64_t sequence = 1; sequence <= StressFrames; ++sequence) {
        unsigned index = (unsigned)((sequence - 1) % 3);
        while (!av_video_begin_write(&slots[index])) {
        }
        slots[index].frame_sequence = sequence;
        pixel_sequences[index] = sequence;
        assert(av_video_publish(&slots[index]));
    }
    return NULL;
}
static void *consume(void *context) {
    (void)context;
    for (uint64_t sequence = 1; sequence <= StressFrames; ++sequence) {
        unsigned index = (unsigned)((sequence - 1) % 3);
        while (!av_video_begin_read(&slots[index])) {
        }
        assert(slots[index].frame_sequence == sequence);
        assert(pixel_sequences[index] == sequence);
        assert(!av_video_begin_write(&slots[index]));
        assert(!av_video_cancel(&slots[index]));
        assert(av_video_release(&slots[index]));
    }
    return NULL;
}
static void *audio_produce(void *context) {
    (void)context;
    for (uint32_t sequence = 1; sequence <= StressFrames;) {
        int64_t count = av_audio_write(&ring, (uint8_t *)pcm, sizeof(pcm),
                                       (const uint8_t *)&sequence, sizeof(sequence));
        assert(count >= 0);
        if (count)
            ++sequence;
    }
    return NULL;
}
static void *audio_consume(void *context) {
    (void)context;
    for (uint32_t sequence = 1; sequence <= StressFrames;) {
        uint32_t frame = 0;
        int64_t count =
            av_audio_read(&ring, (uint8_t *)pcm, sizeof(pcm), (uint8_t *)&frame, sizeof(frame), 0);
        assert(count >= 0);
        if (count) {
            assert(frame == sequence);
            ++sequence;
        } else
            assert(frame == 0);
    }
    return NULL;
}
int main(void) {
    window_slot_header_t slot = {0};
    assert(!av_video_publish(&slot));
    assert(!av_video_begin_read(&slot));
    assert(!av_video_release(&slot));
    assert(!av_video_cancel(&slot));
    assert(av_video_begin_write(&slot));
    assert(!av_video_begin_write(&slot));
    assert(av_video_cancel(&slot));
    assert(av_video_begin_write(&slot));
    assert(av_video_publish(&slot));
    assert(!av_video_publish(&slot));
    assert(av_video_cancel(&slot));
    assert(av_video_begin_write(&slot));
    assert(av_video_publish(&slot));
    assert(av_video_begin_read(&slot));
    assert(!av_video_begin_read(&slot));
    assert(av_video_release(&slot));
    pthread_t producer, consumer;
    assert(pthread_create(&producer, NULL, produce, NULL) == 0);
    assert(pthread_create(&consumer, NULL, consume, NULL) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    ring.sample_rate = 48000;
    ring.channels = 2;
    ring.format = 1;
    ring.capacity_frames = RingFrames;
    atomic_init(&ring.read_head, UINT32_MAX - 128);
    atomic_init(&ring.write_head, UINT32_MAX - 128);
    assert(pthread_create(&producer, NULL, audio_produce, NULL) == 0);
    assert(pthread_create(&consumer, NULL, audio_consume, NULL) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(atomic_load(&ring.write_head) == atomic_load(&ring.read_head));
    void *mapping =
        mmap(NULL, AvMappingBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(av_layout_init(mapping, AvMappingBytes) == 0);
    assert(av_layout_validate(mapping, AvMappingBytes) == 0);
    assert(av_layout_slot(mapping, AvMappingBytes, 15, 2)->buffer_index == 2);
    assert(av_layout_pixels(15, 2) + AvSlotCapacity == AvMappingBytes);
    assert(av_layout_validate(mapping, 64) == -1);
    assert(munmap(mapping, AvMappingBytes) == 0);
    int regular = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(regular >= 0);
    assert(av_dmabuf_export(regular, 0, 4096) == -1);
    assert(av_dmabuf_export(regular, 1, 4096) == -1);
    assert(av_dmabuf_export(regular, 0, 0) == -1);
    close(regular);
    puts("AV transport: 100000 concurrent video/PCM frames, wraparound and bounded mapping passed");
    return 0;
}
