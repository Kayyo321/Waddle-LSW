#include "av_peer.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <errno.h>
#include <sys/socket.h>
#endif
static int would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
static int send_bytes(uintptr_t socket, const uint8_t *bytes, size_t length) {
#ifdef _WIN32
    return send((SOCKET)socket, (const char *)bytes, (int)length, 0);
#else
    return (int)send((int)socket, bytes, length, MSG_NOSIGNAL);
#endif
}
static int receive_bytes(uintptr_t socket, uint8_t *bytes, size_t length) {
#ifdef _WIN32
    return recv((SOCKET)socket, (char *)bytes, (int)length, 0);
#else
    return (int)recv((int)socket, bytes, length, 0);
#endif
}
static int fail_peer(av_peer_t *peer) { peer->failed = 1; return -1; }
int av_peer_send(av_peer_t *peer, const av_message_t *message) {
    if (peer->failed) return -1;
    if (peer->head - peer->tail >= AvPeerQueueFrames) {
        uint8_t check[AvControlBytes];
        return message->type == MsgFrameReady && av_control_encode(message, check, sizeof(check)) == 0
            ? 1 : fail_peer(peer);
    }
    if (av_control_encode(message, peer->outgoing[peer->head % AvPeerQueueFrames],
                          AvControlBytes) != 0)
        return fail_peer(peer);
    ++peer->head;
    return 0;
}
int av_peer_pump(av_peer_t *peer, av_peer_notify_t notify, void *context) {
    if (peer->failed) return -1;
    for (unsigned attempt = 0; attempt < 16 && peer->tail != peer->head; ++attempt) {
        int count =
            send_bytes(peer->socket, peer->outgoing[peer->tail % AvPeerQueueFrames] + peer->sent,
                       AvControlBytes - peer->sent);
        if (count < 0) {
            if (would_block())
                break;
            return fail_peer(peer);
        }
        if (!count)
            return fail_peer(peer);
        peer->sent += (size_t)count;
        if (peer->sent == AvControlBytes) {
            peer->sent = 0;
            ++peer->tail;
        }
    }
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        int count = receive_bytes(peer->socket, peer->incoming + peer->received,
                                  AvControlBytes - peer->received);
        if (count < 0)
            return would_block() ? 0 : fail_peer(peer);
        if (!count) {
            peer->failed = 1; /* Orderly EOF is terminal too; report it once. */
            return peer->received ? -1 : 1;
        }
        peer->received += (size_t)count;
        if (peer->received == AvControlBytes) {
            av_message_t message;
            peer->received = 0;
            if (av_control_decode(peer->incoming, AvControlBytes, &message) != 0 ||
                notify(&message, context) != 0 || peer->failed)
                return fail_peer(peer);
        }
    }
    return 0;
}
