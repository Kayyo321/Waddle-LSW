#include "waddle/av_memory.h"
#include <assert.h>
int main(void) {
    window_slot_header_t slot = {0};
    audio_ring_header_t ring = {0};
    assert(atomic_is_lock_free(&slot.slot_state));
    assert(atomic_is_lock_free(&ring.write_head));
    assert(offsetof(window_slot_header_t, frame_sequence) == 8);
    assert(offsetof(audio_ring_header_t, capacity_frames) == 20);
    return 0;
}
