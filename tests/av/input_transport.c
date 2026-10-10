/** @file input_transport.c @brief Socket-backed input framing and disconnect fixture. */
#include "av_input.h"
#include "av_peer.h"
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct receiver_t {
    av_input_state_t input;
    uint64_t incarnation;
    unsigned emitted, released;
} receiver_t;
static int target(void *context, uint64_t id, uint64_t incarnation, int activate) {
    receiver_t *receiver = context;
    if (id != 7 || incarnation != receiver->incarnation) return activate < 0 ? 1 : -1;
    return 0;
}
static int emit(void *context, const av_message_t *event) {
    receiver_t *receiver = context;
    ++receiver->emitted;
    if (!event->flags && (event->type == MsgInputKey || event->type == MsgInputButton)) ++receiver->released;
    return 0;
}
static const av_input_ops_t Ops = {.target = target, .emit = emit};
static int receive_input(const av_message_t *event, void *context) {
    receiver_t *receiver = context;
    return av_input_apply(&receiver->input, event, &Ops, context);
}
static int no_input(const av_message_t *event, void *context) {
    (void)event; (void)context;
    assert(0); return -1;
}
static void send_held(av_peer_t *host, av_peer_t *guest, receiver_t *receiver) {
    av_message_t event = {.type = MsgInputFocus, .window_id = 7, .sequence = receiver->incarnation,
        .buffer_index = 1, .flags = 1};
    assert(av_peer_send(host, &event) == 0);
    event.type = MsgInputKey; event.width = 30; event.buffer_index = 2;
    assert(av_peer_send(host, &event) == 0);
    event.type = MsgInputButton; event.width = 1; event.buffer_index = 3;
    assert(av_peer_send(host, &event) == 0);
    assert(av_peer_pump(host, no_input, NULL) == 0);
    assert(av_peer_pump(guest, receive_input, receiver) == 0);
    assert(receiver->input.keys[30] && receiver->input.buttons[1]);
}
int main(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
        av_peer_t host = {.socket = (uintptr_t)sockets[0]}, guest = {.socket = (uintptr_t)sockets[1]};
        receiver_t receiver = {.incarnation = 99};
        send_held(&host, &guest, &receiver);
        av_message_t event = {.type = MsgInputPointer, .window_id = 7, .sequence = 99,
            .buffer_index = 4, .x = 10, .y = 20};
        if (mode == 0) {
            for (unsigned i = 0; i < AvPeerQueueFrames; ++i) {
                event.buffer_index = i + 4;
                assert(av_peer_send(&host, &event) == 0);
            }
            event.type = MsgInputRelease; event.flags = 3; event.x = event.y = 0;
            ++event.buffer_index;
            assert(av_peer_send(&host, &event) == -1); /* release cannot enter a full queue */
            assert(close(sockets[0]) == 0); /* host fails closed rather than dropping it */
            assert(av_peer_pump(&guest, receive_input, &receiver) == 1);
        } else if (mode == 1) {
            event.buffer_index = 2; /* replay after focus/key/button already arrived */
            assert(av_peer_send(&host, &event) == 0);
            assert(av_peer_pump(&host, no_input, NULL) == 0);
            assert(av_peer_pump(&guest, receive_input, &receiver) == -1);
            assert(close(sockets[0]) == 0);
        } else {
            uint8_t bytes[AvControlBytes];
            assert(av_control_encode(&event, bytes, sizeof(bytes)) == 0);
            assert(write(sockets[0], bytes, 123) == 123);
            assert(close(sockets[0]) == 0);
            assert(av_peer_pump(&guest, receive_input, &receiver) == -1); /* partial frame EOF */
        }
        assert(receiver.emitted == 2); /* queued/replayed/truncated motion never injects */
        assert(av_input_reset(&receiver.input, &Ops, &receiver) == 0);
        assert(receiver.released == 2 && !receiver.input.window_id);
        assert(close(sockets[1]) == 0);
    }
    // A new connection starts empty; an ordered old-incarnation event is ignored.
    receiver_t fresh = {.incarnation = 100};
    av_message_t focus = {.type = MsgInputFocus, .window_id = 7, .sequence = 100,
        .buffer_index = 1, .flags = 1};
    assert(receive_input(&focus, &fresh) == 0);
    focus.type = MsgInputKey; focus.width = 30; focus.sequence = 99; focus.buffer_index = 2;
    assert(receive_input(&focus, &fresh) == 0 && !fresh.emitted);
    focus.sequence = 100; focus.buffer_index = 3;
    assert(receive_input(&focus, &fresh) == 0 && fresh.emitted == 1);
    assert(av_input_reset(&fresh.input, &Ops, &fresh) == 0 && fresh.released == 1);
    puts("AV input socket framing, full-queue disconnect and fresh-session fixtures passed");
    return 0;
}
