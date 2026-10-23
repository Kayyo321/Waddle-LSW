/** @file session.c @brief Startup, stale wire input, and terminal state tests. */
#include "waddle/venus_session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(64) uint8_t mapping[4096];
static venus_session_t host;
static venus_session_t guest;

static void fresh(void) {
    memset(&host, 0, sizeof(host));
    memset(&guest, 0, sizeof(guest));
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&host, SessionHost, mapping, sizeof(mapping), 123) == RingOk);
    assert(venus_session_init(&guest, SessionGuest, mapping, sizeof(mapping), 0) == RingOk);
    assert(!venus_session_ready(&host));
    assert(!venus_session_ready(&guest));
}

static void closed(const venus_session_t *session, venus_stop_reason_t reason) {
    assert(session->state == SessionClosed);
    assert(session->reason == reason);
    assert(!venus_session_ready(session));
    uint8_t byte = 17;
    assert(venus_ring_write(&host.region.commands, &byte, 1) == RingClosed);
    assert(venus_ring_read(&guest.region.commands, &byte, 1) == RingClosed);
    assert(venus_ring_write(&host.region.replies, &byte, 1) == RingClosed);
    assert(venus_ring_read(&guest.region.replies, &byte, 1) == RingClosed);
    assert(byte == 17);
}

static void handshake(void) {
    uint8_t frame[VenusControlBytes];
    assert(venus_session_send(&host, ControlOffer, StopNone, frame, sizeof(frame)) == RingOk);
    assert(host.state == SessionOffering);
    assert(venus_session_receive(&guest, frame, sizeof(frame)) == RingOk);
    assert(guest.state == SessionAccepted && guest.session_id == 123);
    assert(venus_session_send(&guest, ControlAck, StopNone, frame, sizeof(frame)) == RingOk);
    assert(guest.state == SessionWaiting);
    assert(venus_session_receive(&host, frame, sizeof(frame)) == RingOk);
    assert(host.state == SessionAcknowledged);
    assert(venus_session_send(&host, ControlReady, StopNone, frame, sizeof(frame)) == RingOk);
    assert(venus_session_ready(&host));
    assert(venus_session_receive(&guest, frame, sizeof(frame)) == RingOk);
    assert(venus_session_ready(&guest));
}

static void test_startup_and_stop(void) {
    for (int reason = StopDisconnect; reason <= StopProtocol; reason++) {
        for (int sender = 0; sender < 2; sender++) {
            fresh();
            handshake();
            const uint8_t Command[] = {1, 2, 3};
            uint8_t reply[sizeof(Command)] = {0};
            assert(venus_ring_write(&guest.region.commands, Command, sizeof(Command)) == RingOk);
            assert(venus_ring_read(&host.region.commands, reply, sizeof(reply)) == RingOk);
            assert(memcmp(Command, reply, sizeof(reply)) == 0);
            assert(venus_ring_write(&host.region.replies, reply, sizeof(reply)) == RingOk);
            assert(venus_ring_read(&guest.region.replies, reply, sizeof(reply)) == RingOk);
            uint8_t frame[VenusControlBytes];
            venus_session_t *source = sender ? &host : &guest;
            venus_session_t *target = sender ? &guest : &host;
            assert(venus_session_send(source, ControlStop, (venus_stop_reason_t)reason, frame,
                                      sizeof(frame)) == RingOk);
            closed(source, (venus_stop_reason_t)reason);
            assert(venus_session_receive(target, frame, sizeof(frame)) == RingClosed);
            closed(target, (venus_stop_reason_t)reason);
            venus_session_close(target, StopProtocol);
            closed(target, (venus_stop_reason_t)reason);
            assert(venus_session_receive(target, frame, sizeof(frame)) == RingClosed);
        }
    }
    fresh();
    uint8_t frame[VenusControlBytes];
    assert(venus_session_send(&host, ControlStop, StopCancel, frame, sizeof(frame)) == RingOk);
    assert(venus_session_receive(&guest, frame, sizeof(frame)) == RingCorrupt);
    closed(&guest, StopProtocol); /* Guest has not learned an identity yet. */
}

static void test_init_and_local_errors(void) {
    fresh();
    venus_session_t session = {0};
    assert(venus_session_init(NULL, SessionHost, mapping, sizeof(mapping), 1) == RingInvalid);
    assert(venus_session_init(&session, 0, mapping, sizeof(mapping), 1) == RingInvalid);
    assert(session.state == SessionNone);
    assert(venus_session_init(&session, SessionHost, mapping, sizeof(mapping), 0) == RingInvalid);
    assert(venus_session_init(&session, SessionGuest, mapping, sizeof(mapping), 1) == RingInvalid);
    assert(venus_session_init(&session, SessionGuest, NULL, sizeof(mapping), 0) == RingInvalid);
    assert(venus_session_init(&session, SessionHost, mapping, 63, 1) == RingInvalid);
    _Atomic uint32_t *cursors[] = {
        &host.region.commands.header->head, &host.region.commands.header->tail,
        &host.region.replies.header->head, &host.region.replies.header->tail};
    for (size_t index = 0; index < 4; index++) {
        atomic_store_explicit(cursors[index], 1, memory_order_release);
        assert(venus_session_init(&session, SessionHost, mapping, sizeof(mapping), 1) ==
               RingInvalid);
        assert(session.state == SessionNone);
        atomic_store_explicit(cursors[index], 0, memory_order_release);
    }
    venus_session_close(NULL, StopCancel);
    venus_session_close(&session, StopCancel);
    assert(session.state == SessionNone);
    assert(!venus_session_ready(NULL));
    assert(!venus_session_ready(&session));
    uint8_t frame[VenusControlBytes];
    memset(frame, 0x5a, sizeof(frame));
    assert(venus_session_send(NULL, ControlOffer, StopNone, frame, sizeof(frame)) == RingInvalid);
    assert(venus_session_send(&session, ControlOffer, StopNone, frame, sizeof(frame)) ==
           RingInvalid);
    assert(venus_session_send(&host, ControlOffer, StopNone, NULL, sizeof(frame)) == RingInvalid);
    assert(venus_session_send(&host, ControlOffer, StopNone, frame, 63) == RingInvalid);
    assert(venus_session_send(&host, ControlOffer, StopCancel, frame, sizeof(frame)) ==
           RingInvalid);
    assert(venus_session_send(&guest, ControlStop, StopCancel, frame, sizeof(frame)) ==
           RingInvalid);
    assert(venus_session_send(&host, ControlStop, StopNone, frame, sizeof(frame)) == RingInvalid);
    assert(venus_session_send(&host, ControlStop, 5, frame, sizeof(frame)) == RingInvalid);
    for (size_t index = 0; index < sizeof(frame); index++)
        assert(frame[index] == 0x5a);
    assert(host.state == SessionInitialized);
    assert(venus_session_receive(NULL, frame, sizeof(frame)) == RingInvalid);
    assert(venus_session_receive(&session, frame, sizeof(frame)) == RingInvalid);
    assert(venus_session_receive(&host, NULL, sizeof(frame)) == RingInvalid);
    venus_session_close(&host, 99);
    closed(&host, StopProtocol);
    assert(venus_session_send(&host, ControlStop, StopCancel, frame, sizeof(frame)) == RingInvalid);
}

static void test_transition_matrix(void) {
    uint8_t frame[VenusControlBytes];
    for (int role = SessionHost; role <= SessionGuest; role++) {
        for (int state = SessionInitialized; state <= SessionReady; state++) {
            for (int kind = 0; kind <= ControlStop; kind++) {
                fresh();
                venus_session_t *session = role == SessionHost ? &host : &guest;
                session->state = (venus_session_state_t)state;
                session->session_id = 123;
                memset(frame, 0xa5, sizeof(frame));
                int legal =
                    kind == ControlStop ||
                    (role == SessionHost && state == SessionInitialized && kind == ControlOffer) ||
                    (role == SessionHost && state == SessionAcknowledged && kind == ControlReady) ||
                    (role == SessionGuest && state == SessionAccepted && kind == ControlAck);
                assert(venus_session_send(session, (venus_control_kind_t)kind,
                                          kind == ControlStop ? StopCancel : StopNone, frame,
                                          sizeof(frame)) == (legal ? RingOk : RingInvalid));
                if (!legal) {
                    assert(session->state == (venus_session_state_t)state);
                    for (size_t index = 0; index < sizeof(frame); index++)
                        assert(frame[index] == 0xa5);
                }
                if (kind == 0)
                    continue;
                fresh();
                session = role == SessionHost ? &host : &guest;
                session->state = (venus_session_state_t)state;
                session->session_id =
                    (role == SessionGuest && state == SessionInitialized) ? 0 : 123;
                venus_control_t control = {.kind = (uint32_t)kind,
                                           .reason = kind == ControlStop ? StopCancel : StopNone,
                                           .session_id = 123,
                                           .mapping_bytes = 4096,
                                           .capacity = 64};
                assert(venus_control_encode(&control, frame, sizeof(frame)) == RingOk);
                legal =
                    (role == SessionGuest && state == SessionInitialized && kind == ControlOffer) ||
                    (role == SessionHost && state == SessionOffering && kind == ControlAck) ||
                    (role == SessionGuest && state == SessionWaiting && kind == ControlReady);
                int stop =
                    kind == ControlStop && !(role == SessionGuest && state == SessionInitialized);
                assert(venus_session_receive(session, frame, sizeof(frame)) ==
                       (legal ? RingOk : (stop ? RingClosed : RingCorrupt)));
                if (!legal)
                    closed(session, stop ? StopCancel : StopProtocol);
            }
        }
    }
}

static void test_stale_and_corrupt_frames(void) {
    uint8_t frame[VenusControlBytes];
    for (int field = 0; field < 5; field++) {
        fresh();
        venus_control_t control = {.kind = ControlAck,
                                   .reason = StopNone,
                                   .session_id = 123,
                                   .mapping_bytes = 4096,
                                   .capacity = 64};
        host.state = SessionOffering;
        if (field == 0)
            control.session_id++;
        if (field == 1)
            control.mapping_bytes *= 2;
        if (field == 2)
            control.capacity *= 2;
        assert(venus_control_encode(&control, frame, sizeof(frame)) == RingOk);
        if (field == 3)
            frame[0] ^= 1;
        assert(venus_session_receive(&host, frame, field == 4 ? 63 : sizeof(frame)) == RingCorrupt);
        closed(&host, StopProtocol);
    }
    fresh();
    handshake();
    venus_control_t duplicate = {.kind = ControlReady,
                                 .reason = StopNone,
                                 .session_id = 123,
                                 .mapping_bytes = 4096,
                                 .capacity = 64};
    assert(venus_control_encode(&duplicate, frame, sizeof(frame)) == RingOk);
    assert(venus_session_receive(&guest, frame, sizeof(frame)) == RingCorrupt);
    closed(&guest, StopProtocol);
}

int main(void) {
    test_init_and_local_errors();
    test_startup_and_stop();
    test_transition_matrix();
    test_stale_and_corrupt_frames();
    puts("Venus session handoff, transitions, stale input, and closure passed");
    return 0;
}
