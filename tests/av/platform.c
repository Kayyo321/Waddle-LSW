/** @file platform.c @brief Real compositor and PipeWire callback/release gate. */
#include "av_layout.h"
#include "av_pipewire.h"
#include "av_wayland.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int requests;
static unsigned commits;
static void observed_commit(void *context) {
    assert(context == &commits);
    ++commits;
}
static int host_request(const av_message_t *message, void *context) {
    (void)context;
    assert(message->type == MsgWindowGeometry || message->type == MsgWindowClose);
    ++requests;
    return 0;
}
int main(void) {
    int memory = memfd_create("waddle-av-platform-test", MFD_CLOEXEC);
    assert(memory >= 0 && ftruncate(memory, AvMappingBytes) == 0);
    uint8_t *mapping = mmap(NULL, AvMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
    assert(mapping != MAP_FAILED && av_layout_init(mapping, AvMappingBytes) == 0);
    av_wayland_t *video = NULL;
    assert(av_wayland_init(&video, host_request, NULL) == 0);
    audio_ring_header_t *ring = (audio_ring_header_t *)(mapping + AvAudioHeaderOffset);
    av_pipewire_t audio = {0};
    assert(av_pipewire_init(&audio, ring, mapping + AvPcmOffset, AvAudioCapacity * AvAudioFrameBytes) == 0);
    window_slot_header_t *slots[3];
    uint64_t offsets[3];
    for (unsigned i = 0; i < 3; ++i) {
        slots[i] = av_layout_slot(mapping, AvMappingBytes, 0, i);
        offsets[i] = av_layout_pixels(0, i);
    }
    av_message_t message = {.type = MsgWindowCreate, .window_id = 4242, .width = 320,
        .height = 200, .dpi = 96, .process_id = 1};
    strcpy(message.title, "Waddle AV native validation");
    assert(av_wayland_create(video, &message, memory, AvUsedBytes, slots, offsets, AvSlotCapacity) == 0);
    assert(av_wayland_watch(video, 4242, 0x888888, mapping, AvMappingBytes,
                             observed_commit, &commits) == 0);
    assert(av_wayland_watch(video, 4242, 0x888888, mapping, AvMappingBytes,
                             observed_commit, &commits) == -1);
    uint8_t silence[1024] = {0};
    int submitted = 0;
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        assert(av_pipewire_ready(&audio));
        assert(av_audio_write(ring, mapping + AvPcmOffset, AvAudioCapacity * AvAudioFrameBytes,
                              silence, sizeof(silence)) >= 0);
        unsigned index = iteration % 3;
        window_slot_header_t *slot = slots[index];
        if (av_video_begin_write(slot)) {
            memset(mapping + offsets[index], 0x88, 320 * 200 * 4);
            slot->width = 320; slot->height = 200; slot->stride = 1280; slot->format = AvPixelFormat;
            slot->frame_sequence = iteration + 1;
            assert(av_video_publish(slot));
            message.type = MsgFrameReady;
            message.buffer_index = index; message.sequence = iteration + 1;
            message.damage_width = 320; message.damage_height = 200;
            assert(av_wayland_message(video, &message) == 0);
            submitted |= atomic_load_explicit(&slot->slot_state, memory_order_acquire) == SlotConsuming;
        }
        struct pollfd display = {av_wayland_fd(video), POLLIN | (av_wayland_writable(video) ? POLLOUT : 0), 0};
        assert(poll(&display, 1, 10) >= 0);
        if (display.revents & POLLIN) assert(av_wayland_dispatch(video) == 0);
        assert(av_wayland_flush(video) == 0);
    }
    fprintf(stderr, "submitted=%d audio_state=%d read=%u underrun=%u invalid=%u\n", submitted, atomic_load(&audio.state), atomic_load(&ring->read_head), atomic_load(&audio.underrun_frames), atomic_load(&audio.invalid_buffers));
    assert(commits == 1);
    assert(submitted && atomic_load_explicit(&ring->read_head, memory_order_acquire) != 0);
    av_wayland_free(&video);
    for (unsigned i = 0; i < 3; ++i)
        assert(atomic_load_explicit(&slots[i]->slot_state, memory_order_acquire) == SlotFree);
    av_pipewire_free(&audio);
    assert(munmap(mapping, AvMappingBytes) == 0);
    close(memory);
    printf("Native platform: real surface attachment/release and connected PCM callbacks passed (%d geometry requests)\n", requests);
    return 0;
}
