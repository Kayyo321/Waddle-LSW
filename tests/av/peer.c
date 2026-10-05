#include "av_peer.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
static int notifications;
static int receive_message(const av_message_t *message, void *context) {
    (void)context;
    assert(message->type == MsgWindowClose && message->window_id == 42);
    ++notifications;
    return 0;
}
int main(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    av_peer_t sender = {.socket = (uintptr_t)sockets[0]},
              receiver = {.socket = (uintptr_t)sockets[1]};
    av_message_t message = {.type = MsgWindowClose, .window_id = 42};
    uint8_t bytes[AvControlBytes];
    assert(av_control_encode(&message, bytes, sizeof(bytes)) == 0);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        assert(write(sockets[0], bytes + i, 1) == 1);
        assert(av_peer_pump(&receiver, receive_message, NULL) == 0);
        assert(notifications == (i + 1 == sizeof(bytes) ? 1 : 0));
    }
    for (unsigned i = 0; i < AvPeerQueueFrames; ++i)
        assert(av_peer_send(&sender, &message) == 0);
    assert(av_peer_send(&sender, &message) == 1);
    for (unsigned i = 0; i < 64 && notifications < 129; ++i) {
        assert(av_peer_pump(&sender, receive_message, NULL) == 0);
        assert(av_peer_pump(&receiver, receive_message, NULL) == 0);
    }
    assert(notifications == 129 && sender.head == sender.tail);
    bytes[0] ^= 0xff;
    assert(write(sockets[0], bytes, sizeof(bytes)) == sizeof(bytes));
    assert(av_peer_pump(&receiver, receive_message, NULL) == -1);
    close(sockets[0]);
    assert(av_peer_pump(&receiver, receive_message, NULL) == -1);
    close(sockets[1]);
    puts("AV peer: one-byte fragmented frames, bounded burst backpressure, corruption and EOF "
         "passed");
    return 0;
}
