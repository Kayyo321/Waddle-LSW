/** @file guest_quiescence.c @brief Execute production guest scheduling with a capture API double.
 * Native Windows CI runs this without a GPU, driver, COM initialization or desktop
 * input. A reserved address range backs only the small shared metadata prefix.
 */
#include <winsock2.h>
#include "av_capture.h"
#include "av_windows.h"
#include <assert.h>
static unsigned capture_calls, free_calls;
static int tracker_status;
static HRESULT fixture_capture_init(av_capture_t *capture, HWND window) {
    (void)capture; (void)window; assert(0); return E_FAIL;
}
static HRESULT fixture_capture_frame(av_capture_t *capture, const RECT *bounds,
    window_slot_header_t *slot, uint8_t *pixels, size_t capacity) {
    (void)capture; (void)bounds; (void)pixels; (void)capacity;
    ++capture_calls;
    slot->width = 16; slot->height = 16; slot->frame_sequence = 1;
    atomic_store(&slot->slot_state, SlotReady);
    return S_OK;
}
static void fixture_capture_free(av_capture_t *capture) { (void)capture; ++free_calls; }
static int fixture_tracker_status(void) { return tracker_status; }
#define av_capture_init fixture_capture_init
#define av_capture_frame fixture_capture_frame
#define av_capture_free fixture_capture_free
#define av_windows_status fixture_tracker_status
#define main av_guest_fixture_unused_main
#include "av_guest.c"
#undef main
int main(void) {
    uint8_t *mapping = VirtualAlloc(NULL, AvMappingBytes, MEM_RESERVE, PAGE_READWRITE);
    assert(mapping && VirtualAlloc(mapping, AvPixelOffset, MEM_COMMIT, PAGE_READWRITE) == mapping);
    assert(av_layout_init(mapping, AvMappingBytes) == 0);
    av_ivshmem_t memory = {.mapping = mapping, .length = AvMappingBytes};
    guest_av_t session = {.memory = &memory};
    session.stop = CreateEventW(NULL, TRUE, FALSE, NULL); assert(session.stop);
    av_message_t create = {.type = MsgWindowCreateV2, .window_id = 42, .sequence = 10,
        .width = 16, .height = 16, .dpi = 96, .process_id = 100};
    session.peer.head = AvPeerQueueFrames;
    assert(window_notification(&create, &session) == -1 && session.failed && session.peer.failed);
    assert(!session.windows[0].active && !free_calls);
    assert(WaitForSingleObject(session.stop, 0) == WAIT_OBJECT_0);
    capture_windows(&session); assert(!capture_calls);
    assert(window_notification(&create, &session) == -1);
    create.type = MsgWindowClose;
    assert(host_request(&create, &session) == -1 && !capture_calls);
    session.failed = 0; session.peer = (av_peer_t){0}; assert(ResetEvent(session.stop));
    tracker_status = -1; capture_windows(&session); assert(!capture_calls); tracker_status = 0;
    create.type = MsgWindowCreateV2;
    for (unsigned i = 0; i < 2; ++i) {
        session.windows[i].active = 1;
        session.windows[i].geometry = create;
        session.windows[i].geometry.window_id += i;
        session.windows[i].capture.wgc = (av_wgc_t *)(uintptr_t)1;
    }
    /* A negative enqueue after pool0 capture must return before pool1 capture. */
    session.peer.failed = 1;
    capture_windows(&session);
    assert(capture_calls == 1 && session.failed && !free_calls);
    for (unsigned i = 0; i < 2; ++i)
        assert(atomic_load(&av_layout_slot(mapping, AvMappingBytes, i, 0)->slot_state) == SlotFree);
    assert(WaitForSingleObject(session.stop, 0) == WAIT_OBJECT_0);
    capture_windows(&session); assert(capture_calls == 1);
    /* The existing full-video drop policy remains nonterminal for both pools. */
    session.failed = 0; session.peer = (av_peer_t){.head = AvPeerQueueFrames};
    assert(ResetEvent(session.stop));
    capture_windows(&session);
    assert(capture_calls == 3 && !session.failed && !session.peer.failed);
    assert(WaitForSingleObject(session.stop, 0) == WAIT_TIMEOUT);
    assert(CloseHandle(session.stop) && VirtualFree(mapping, 0, MEM_RELEASE));
    puts("AV guest quiescence: lifecycle failure, disabled tracker and terminal capture enqueue passed");
    return 0;
}
