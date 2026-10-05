#include "av_transport.h"

static int transition(window_slot_header_t *slot, uint32_t from, uint32_t to,
                      memory_order order) {
    return atomic_compare_exchange_strong_explicit(&slot->slot_state, &from, to,
                                                   order, memory_order_acquire);
}
int av_video_begin_write(window_slot_header_t *slot) {
    return transition(slot, SlotFree, SlotWriting, memory_order_acq_rel);
}
int av_video_publish(window_slot_header_t *slot) {
    atomic_thread_fence(memory_order_release);
    return transition(slot, SlotWriting, SlotReady, memory_order_acq_rel);
}
int av_video_begin_read(window_slot_header_t *slot) {
    int claimed = transition(slot, SlotReady, SlotConsuming, memory_order_acq_rel);
    if (claimed) atomic_thread_fence(memory_order_acquire);
    return claimed;
}
int av_video_release(window_slot_header_t *slot) {
    return transition(slot, SlotConsuming, SlotFree, memory_order_acq_rel);
}
int av_video_cancel(window_slot_header_t *slot) {
    if (transition(slot, SlotWriting, SlotFree, memory_order_acq_rel)) return 1;
    return transition(slot, SlotReady, SlotFree, memory_order_acq_rel);
}
