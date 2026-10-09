/** @file lifecycle_transport.c @brief Real FIFO/fragmentation and upgrade-direction fixtures. */
#include "av_identity.h"
#include "av_peer.h"
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
/** @brief Historical codec and peer, test-only ABI-identical message structure. */
extern int legacy_av_control_encode(const av_message_t *message, uint8_t *bytes, size_t capacity);
/** @brief Historical host pump borrowing one socket; -1 rejects an unknown type. */
extern int av_legacy_pump(uintptr_t socket);
_Static_assert(sizeof(av_message_t) == 328, "historical decoded message ABI");
typedef struct endpoint_t {
    av_identity_session_t identity;
    av_message_t current;
    unsigned received, effects;
} endpoint_t;
static int apply_effect(const av_message_t *message, void *context) {
    endpoint_t *endpoint = context;
    assert(message->sequence == endpoint->current.sequence);
    ++endpoint->effects;
    return 0;
}
static int receive_control(const av_message_t *message, void *context) {
    endpoint_t *endpoint = context;
    ++endpoint->received;
    return av_identity_apply(endpoint->current.window_id ? &endpoint->current : NULL,
                             message, apply_effect, endpoint);
}
static int receive_lifecycle(const av_message_t *message, void *context) {
    endpoint_t *endpoint = context;
    ++endpoint->received;
    if (message->type == MsgWindowCreate || message->type == MsgWindowCreateV2) {
        if (endpoint->current.window_id || av_identity_admit(&endpoint->identity, message)) return -1;
        endpoint->current = *message;
        return 0;
    }
    if (message->type != MsgWindowGeometry && message->type != MsgWindowDestroy) return -1;
    if (endpoint->identity.mode == AvIdentityModern) {
        int match = av_identity_match(&endpoint->current, message);
        if (match != 1) return match;
    }
    if (message->type == MsgWindowDestroy) endpoint->current.window_id = 0;
    else endpoint->current = *message;
    return 0;
}
static int unexpected(const av_message_t *message, void *context) {
    (void)message; (void)context; assert(0); return -1;
}
static void socket_pair(int sockets[2], av_peer_t *host, av_peer_t *guest) {
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    *host = (av_peer_t){.socket = (uintptr_t)sockets[0]};
    *guest = (av_peer_t){.socket = (uintptr_t)sockets[1]};
}
static void fragmented(int fd, av_peer_t *receiver, const uint8_t bytes[AvControlBytes], endpoint_t *state) {
    unsigned before = state->received;
    for (size_t i = 0; i < AvControlBytes; ++i) {
        assert(write(fd, bytes + i, 1) == 1);
        assert(av_peer_pump(receiver, receive_lifecycle, state) == 0);
        assert(state->received == before + (i + 1 == AvControlBytes));
    }
}
static unsigned overflowing_callbacks;
static int overflow_during_callback(const av_message_t *message, void *context) {
    av_peer_t *peer = context;
    ++overflowing_callbacks;
    for (unsigned i = 0; i < AvPeerQueueFrames; ++i) assert(av_peer_send(peer, message) == 0);
    assert(av_peer_send(peer, message) == -1);
    return 0; /* The pump must honor its own latch even if a callback misses it. */
}
int main(void) {
    av_message_t create = {.type = MsgWindowCreateV2, .window_id = 42, .sequence = 10,
        .width = 640, .height = 480, .dpi = 96, .process_id = 100};
    /* The historical host really decodes type13, fails, closes, and new guest sees EOF. */
    int sockets[2]; av_peer_t host, guest;
    socket_pair(sockets, &host, &guest);
    assert(av_peer_send(&guest, &create) == 0);
    assert(av_peer_pump(&guest, unexpected, NULL) == 0);
    assert(av_legacy_pump((uintptr_t)sockets[0]) == -1);
    assert(shutdown(sockets[0], SHUT_RDWR) == 0 && close(sockets[0]) == 0);
    assert(av_peer_pump(&guest, unexpected, NULL) == 1 && guest.failed);
    assert(av_peer_send(&guest, &create) == -1);
    assert(av_peer_pump(&guest, unexpected, NULL) == -1);
    close(sockets[1]);
    /* Both pre-input and input-era old guests, using the real old encoder. */
    for (unsigned variant = 0; variant < 2; ++variant) {
        socket_pair(sockets, &host, &guest);
        endpoint_t endpoint = {0}; uint8_t bytes[AvControlBytes];
        av_message_t legacy = create; legacy.type = MsgWindowCreate; legacy.sequence = variant ? 55 : 0;
        assert(legacy_av_control_encode(&legacy, bytes, sizeof(bytes)) == 0);
        fragmented(sockets[1], &host, bytes, &endpoint);
        assert(endpoint.identity.mode == AvIdentityLegacy && !endpoint.identity.high_water);
        legacy.type = MsgWindowGeometry; legacy.sequence = 0;
        assert(legacy_av_control_encode(&legacy, bytes, sizeof(bytes)) == 0);
        fragmented(sockets[1], &host, bytes, &endpoint);
        assert(endpoint.identity.mode == AvIdentityLegacy && !endpoint.effects && host.head == 0);
        legacy.type = MsgWindowDestroy;
        assert(legacy_av_control_encode(&legacy, bytes, sizeof(bytes)) == 0);
        fragmented(sockets[1], &host, bytes, &endpoint);
        assert(!endpoint.current.window_id && !endpoint.effects && host.head == 0);
        close(sockets[0]); close(sockets[1]);
    }
    /* Cross-direction race: old host controls are in flight when A retires and B admits. */
    socket_pair(sockets, &host, &guest);
    endpoint_t host_state = {0}, guest_state = {.current = create};
    assert(av_peer_send(&guest, &create) == 0);
    assert(av_peer_pump(&guest, receive_control, &guest_state) == 0);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0);
    av_message_t old = create; old.type = MsgWindowClose;
    assert(av_peer_send(&host, &old) == 0);
    old.type = MsgWindowGeometry;
    for (unsigned flags = 0; flags <= 3; ++flags) {
        old.flags = flags; assert(av_peer_send(&host, &old) == 0);
    }
    av_message_t destroy = {.type = MsgWindowDestroy, .window_id = 42, .sequence = 10};
    assert(av_peer_send(&guest, &destroy) == 0);
    create.sequence = 20; guest_state.current = create;
    assert(av_peer_send(&guest, &create) == 0);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0); /* send old controls */
    assert(av_peer_pump(&guest, receive_control, &guest_state) == 0); /* send A destroy/B create first */
    assert(guest_state.received == 5 && !guest_state.effects);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0);
    assert(host_state.current.sequence == 20);
    assert(av_peer_send(&guest, &destroy) == 0); /* late duplicate A destroy */
    assert(av_peer_pump(&guest, receive_control, &guest_state) == 0);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0 && host_state.current.sequence == 20);
    old.sequence = 20;
    assert(av_peer_send(&host, &old) == 0);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0);
    assert(av_peer_pump(&guest, receive_control, &guest_state) == 0 && guest_state.effects == 1);
    close(sockets[0]); close(sockets[1]);
    /* Required queues fail terminally; only valid video may be dropped. */
    for (unsigned kind = 0; kind < 4; ++kind) {
        socket_pair(sockets, &host, &guest);
        av_message_t message = create;
        if (kind == 0) message.type = MsgWindowClose;
        if (kind == 1) message.type = MsgWindowGeometry;
        if (kind == 2) message.type = MsgWindowDestroy;
        if (kind == 3) message = (av_message_t){.type = MsgInputFocus, .window_id = 42,
            .sequence = 20, .buffer_index = 1, .flags = 1};
        for (unsigned i = 0; i < AvPeerQueueFrames; ++i) assert(av_peer_send(&guest, &message) == 0);
        av_message_t frame = create; frame.type = MsgFrameReady;
        frame.damage_width = frame.width; frame.damage_height = frame.height;
        assert(av_peer_send(&guest, &frame) == 1 && !guest.failed);
        assert(av_peer_send(&guest, &message) == -1 && guest.failed);
        uint32_t head = guest.head;
        assert(av_peer_send(&guest, &frame) == -1 && guest.head == head);
        assert(av_peer_pump(&guest, unexpected, NULL) == -1);
        uint8_t byte; assert(read(sockets[0], &byte, 1) == -1); /* no post-failure I/O */
        close(sockets[0]); close(sockets[1]);
    }
    /* Native send failure and partial EOF are terminal; a fresh connection resets. */
    socket_pair(sockets, &host, &guest);
    close(sockets[0]); assert(av_peer_send(&guest, &create) == 0);
    assert(av_peer_pump(&guest, unexpected, NULL) == -1 && guest.failed);
    assert(av_peer_send(&guest, &create) == -1); close(sockets[1]);
    socket_pair(sockets, &host, &guest);
    uint8_t bytes[AvControlBytes]; assert(av_control_encode(&create, bytes, sizeof(bytes)) == 0);
    assert(write(sockets[1], bytes, 127) == 127); close(sockets[1]);
    assert(av_peer_pump(&host, unexpected, NULL) == -1 && host.failed); close(sockets[0]);
    socket_pair(sockets, &host, &guest); host_state = (endpoint_t){0}; create.sequence = 1;
    assert(av_peer_send(&guest, &create) == 0 && !guest.failed);
    assert(av_peer_pump(&guest, unexpected, NULL) == 0);
    assert(av_peer_pump(&host, receive_lifecycle, &host_state) == 0 && host_state.current.sequence == 1);
    close(sockets[0]); close(sockets[1]);
    socket_pair(sockets, &host, &guest);
    assert(av_control_encode(&create, bytes, sizeof(bytes)) == 0);
    assert(write(sockets[1], bytes, sizeof(bytes)) == sizeof(bytes));
    assert(write(sockets[1], bytes, sizeof(bytes)) == sizeof(bytes));
    assert(av_peer_pump(&host, overflow_during_callback, &host) == -1);
    assert(overflowing_callbacks == 1 && host.failed && !host.received);
    assert(read(sockets[0], bytes, sizeof(bytes)) == sizeof(bytes)); /* second frame remains unread */
    close(sockets[0]); close(sockets[1]);
    puts("AV lifecycle transport: both upgrade directions, fragmented FIFO races and terminal failure passed");
    return 0;
}
