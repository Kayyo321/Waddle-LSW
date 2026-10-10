/** @file lease.c @brief Production lease/core FIFO, deadline and cleanup regressions. */
#include "av_lease.h"
#include "av_identity.h"
#include "av_peer.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
/** @brief Frozen V2 ABI prefix parser; only historical bytes are read/written. */
extern int v2_av_control_decode(const uint8_t *, size_t, void *);
extern int v2_av_control_encode(const void *, uint8_t *, size_t);
typedef struct fixture_t {
    av_lease_guest_t guest;
    av_lease_host_t host;
    uint64_t now, live_id, live_incarnation, transition_id;
    unsigned published, probes, activations, emitted, released, reconciled;
    int fail_publish, fail_target, fail_activation, fail_emit, fail_reconcile, suspend_emit;
    int transition_after_emit, transition_before_effect, clock_after_publish;
    uint32_t fail_release;
    av_message_t last;
    av_peer_t *output;
} fixture_t;
static const av_lease_ops_t Ops;
static uint64_t now_ms(void *context) { return ((fixture_t *)context)->now; }
static int target(void *context, uint64_t id, uint64_t incarnation, int activate) {
    fixture_t *f = context;
    if (activate < 0) ++f->probes;
    if (f->fail_target) return -1;
    if (id != f->live_id || incarnation != f->live_incarnation) return activate < 0 ? 1 : -1;
    if (activate > 0) { ++f->activations; if (f->fail_activation) return -1; }
    return 0;
}
static int emit(void *context, const av_message_t *event) {
    fixture_t *f = context;
    if (!event->buffer_index) {
        ++f->released;
        return f->fail_release == event->width ? -1 : 0;
    }
    if (f->fail_emit) return -1;
    if (f->suspend_emit) { f->transition_id = 8; return 1; }
    ++f->emitted;
    if (f->transition_after_emit) f->transition_id = 8;
    return 0;
}
static int publish(void *context, const av_message_t *message) {
    fixture_t *f = context; ++f->published; f->last = *message;
    if (f->clock_after_publish) f->now = 0;
    if (f->fail_publish) return f->fail_publish;
    return f->output ? av_peer_send(f->output, message) : 0;
}
static int reconcile(void *context) {
    fixture_t *f = context; ++f->reconciled;
    if (f->fail_reconcile) return -1;
    if (f->transition_before_effect) { f->transition_before_effect = 0; f->transition_id = 8; }
    if (f->transition_id) {
        f->live_id = f->transition_id; f->transition_id = 0;
        return av_lease_guest_transition(&f->guest, f->live_id, f->live_incarnation, &Ops, f);
    }
    return 0;
}
static const av_lease_ops_t Ops = {.input = {.target = target, .emit = emit},
    .publish = publish, .reconcile = reconcile, .now_ms = now_ms};
static fixture_t fixture(void) {
    return (fixture_t){.now = 100, .live_id = 7, .live_incarnation = 99};
}
static av_message_t input(fixture_t *f, uint32_t type, uint32_t flags, uint32_t code) {
    return (av_message_t){.type = type, .flags = flags, .width = code,
        .window_id = f->live_id, .sequence = f->live_incarnation,
        .buffer_index = f->guest.input.serial + 1, .lease_generation = f->guest.epoch};
}
static void handshake(fixture_t *f) {
    assert(av_lease_guest_begin(&f->guest, &Ops, f) == 0);
    assert(f->last.type == MsgInputEpochRevoked && f->last.lease_generation == 1);
    assert(av_lease_host_receive(&f->host, &f->last, f->now) == 1);
    av_message_t ack;
    assert(av_lease_host_ack(&f->host, f->now, &ack) == 0);
    assert(av_lease_guest_apply(&f->guest, &ack, &Ops, f) == 0);
    assert(f->last.type == MsgInputEpochReady && f->last.buffer_index == ack.buffer_index);
    assert(av_lease_host_receive(&f->host, &f->last, f->now) == 0);
    assert(f->host.phase == AvLeaseHostUnfocusedReady && f->guest.phase == AvLeaseUnfocusedReady);
    assert(!f->guest.ever_anchored && !f->guest.input.window_id && !f->emitted);
}
static void focus(fixture_t *f) {
    av_message_t event = input(f, MsgInputFocusV3, 1, 0);
    assert(av_lease_host_stamp(&f->host, &event) == 0);
    f->host.phase = AvLeaseForwarding;
    assert(av_lease_guest_apply(&f->guest, &event, &Ops, f) == 0);
    assert(f->guest.phase == AvLeaseActive && f->guest.ever_anchored);
    assert(f->guest.input.window_id == 7 && f->guest.anchor_id == 7);
}
static void apply(fixture_t *f, uint32_t type, uint32_t flags, uint32_t code) {
    av_message_t event = input(f, type, flags, code);
    assert(av_lease_guest_apply(&f->guest, &event, &Ops, f) == 0);
}
static void startup_and_ordering(void) {
    fixture_t f = fixture();
    assert(av_lease_guest_transition(&f.guest, 0, 0, &Ops, &f) == 0);
    handshake(&f);
    apply(&f, MsgInputKeyV3, 1, 30);
    apply(&f, MsgInputFocusV3, 0, 0);
    apply(&f, MsgInputReleaseV3, 3, 0);
    av_message_t event = input(&f, MsgInputFocusV3, 1, 0); event.sequence = 98;
    assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == 0);
    assert(!f.activations && !f.emitted && !f.guest.ever_anchored);
    f.host.serial = f.guest.input.serial;
    focus(&f);
    apply(&f, MsgInputPointerV3, 0, 0);
    apply(&f, MsgInputKeyV3, 1, 30);
    assert(f.emitted == 2 && f.guest.input.keys[30]);
    event = input(&f, MsgInputFocusV3, 1, 0); event.sequence = 98;
    assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == 0 && f.guest.input.keys[30]);
    event = input(&f, MsgInputKeyV3, 0, 30); event.window_id = 6;
    assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == 0 && f.guest.input.keys[30]);
    event = input(&f, MsgInputReleaseV3, 3, 0); event.sequence = 98;
    assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == 0 && f.guest.input.keys[30]);
    apply(&f, MsgInputReleaseV3, 1, 0);
    assert(!f.guest.input.keys[30]);
    apply(&f, MsgInputButtonV3, 1, 1);
    apply(&f, MsgInputFocusV3, 0, 0);
    assert(f.guest.phase == AvLeaseUnfocusedReady && f.guest.ever_anchored && !f.guest.input.buttons[1]);
    f.live_id = 8;
    assert(av_lease_guest_transition(&f.guest, 8, 99, &Ops, &f) == 0);
    assert(f.guest.epoch == 2 && f.guest.phase == AvLeaseAwaitAck);
    assert(av_lease_guest_transition(&f.guest, 8, 99, &Ops, &f) == 0);
    f.live_id = 7;
    assert(av_lease_guest_transition(&f.guest, 7, 99, &Ops, &f) == -1);
    assert(f.guest.phase == AvLeaseGuestTerminal && !f.guest.input.window_id);
}
static void stale_and_invalid(void) {
    for (unsigned kind = 0; kind < 12; ++kind) {
        fixture_t f = fixture(); handshake(&f); focus(&f);
        av_message_t event = input(&f, MsgInputKeyV3, 1, 30);
        switch (kind) {
            case 0: event.type = MsgInputKey; event.lease_generation = 0; break;
            case 1: event.type = MsgInputEpochReady; break;
            case 2: event.buffer_index = f.guest.input.serial; break;
            case 3: event.lease_generation = 2; break;
            case 4: event.height = 1; break;
            case 5: f.guest.phase = AvLeaseInitial; break;
            case 6: f.guest.phase = AvLeaseAwaitAck; break;
            case 7: f.guest.phase = AvLeaseGuestTerminal; break;
            case 8: f.fail_reconcile = 1; break;
            case 9: f.fail_emit = 1; break;
            case 10: event.width = 84; break;
            case 11: f.now = 0; break;
        }
        assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == -1);
        assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == -1);
    }
    fixture_t f = fixture(); handshake(&f); focus(&f);
    assert(av_lease_guest_apply(&f.guest, NULL, &Ops, &f) == -1);
    f = fixture(); handshake(&f); focus(&f); f.live_id = 8;
    assert(av_lease_guest_transition(&f.guest, 8, 99, &Ops, &f) == 0);
    unsigned effects = f.reconciled + f.emitted + f.probes + f.released;
    av_message_t old = input(&f, MsgInputFocusV3, 1, 0); old.lease_generation = 1;
    assert(av_lease_guest_apply(&f.guest, &old, &Ops, &f) == 0);
    assert(effects == f.reconciled + f.emitted + f.probes + f.released);
    old.type = MsgInputKeyV3; old.width = 30; ++old.buffer_index;
    assert(av_lease_guest_apply(&f.guest, &old, &Ops, &f) == 0);
    assert(av_lease_guest_apply(&f.guest, &old, &Ops, &f) == -1);
    f = fixture(); handshake(&f); focus(&f);
    old = input(&f, MsgInputKeyV3, 1, 30); old.buffer_index = UINT32_MAX;
    assert(av_lease_guest_apply(&f.guest, &old, &Ops, &f) == 0);
    old.buffer_index = 1; assert(av_lease_guest_apply(&f.guest, &old, &Ops, &f) == -1);
}
static void releases_and_transition(void) {
    for (unsigned kind = 0; kind < 4; ++kind) {
        fixture_t f = fixture(); handshake(&f); focus(&f);
        apply(&f, MsgInputKeyV3, 1, 30);
        apply(&f, MsgInputButtonV3, 1, 1);
        unsigned inserted = f.emitted;
        if (kind == 0) f.transition_before_effect = 1;
        if (kind == 1) f.transition_after_emit = 1;
        if (kind == 2) f.suspend_emit = 1;
        av_message_t event = input(&f, MsgInputKeyV3, kind == 3 ? 0 : 1, kind == 3 ? 30 : 31);
        if (kind == 3) f.transition_after_emit = 1;
        assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == 0);
        assert(f.guest.epoch == 2 && f.guest.phase == AvLeaseAwaitAck);
        assert(!f.guest.input.keys[30] && !f.guest.input.keys[31] && !f.guest.input.buttons[1]);
        assert(f.emitted == inserted + (kind == 1 || kind == 3));
        assert(f.released == (kind == 1 ? 3u : kind == 3 ? 1u : 2u));
    }
    for (unsigned kind = 0; kind < 7; ++kind) {
        fixture_t f = fixture(); handshake(&f); focus(&f);
        apply(&f, MsgInputKeyV3, 1, 30); apply(&f, MsgInputKeyV3, 1, 31);
        if (kind == 0) f.fail_release = 30;
        if (kind == 1) f.fail_target = 1;
        if (kind == 2) f.guest.epoch = UINT64_MAX;
        if (kind == 3) f.fail_publish = 1;
        if (kind == 4) f.now = UINT64_MAX - 1;
        if (kind == 5) f.clock_after_publish = 1;
        f.live_id = 8;
        assert(av_lease_guest_transition(&f.guest, kind == 6 ? 0 : 8, 99, &Ops, &f) == -1);
        if (kind == 0) assert(f.guest.input.keys[30] && !f.guest.input.keys[31]);
        f.fail_release = 0; assert(av_input_reset(&f.guest.input, &Ops.input, &f) == 0);
    }
    fixture_t f = fixture(); handshake(&f); focus(&f);
    assert(av_lease_guest_transition(&f.guest, 7, 0, &Ops, &f) == -1);
    for (unsigned kind = 0; kind < 4; ++kind) {
        f = fixture(); handshake(&f);
        if (kind == 0) f.fail_target = 1;
        if (kind == 1) f.fail_activation = 1;
        if (kind == 2) { f.guest.input.keys[30] = 1; f.fail_release = 30; }
        if (kind == 3) f.fail_reconcile = 1;
        av_message_t event = input(&f, MsgInputFocusV3, 1, 0);
        assert(av_lease_guest_apply(&f.guest, &event, &Ops, &f) == -1);
        assert(!f.guest.ever_anchored && !f.emitted);
    }
}
static void deadlines_and_handshakes(void) {
    assert(av_lease_timeout(0, 100, 1000) == 1000);
    assert(av_lease_timeout(110, 100, 1000) == 10);
    assert(av_lease_timeout(110, 100, 5) == 5);
    assert(av_lease_timeout(110, 110, 5) == -1);
    assert(av_lease_timeout(0, 0, 5) == -1);
    assert(av_lease_timeout(0, 100, -1) == -1);
    for (unsigned kind = 0; kind < 7; ++kind) {
        fixture_t f = fixture();
        if (kind == 0) f.fail_publish = -1;
        if (kind == 1) f.now = 0;
        if (kind == 2) f.now = UINT64_MAX;
        if (kind == 3) f.clock_after_publish = 1;
        if (kind == 4) f.guest.epoch = 1;
        if (kind == 5) f.guest.phase = AvLeaseAwaitAck;
        if (kind == 6) f.guest.epoch = UINT64_MAX;
        assert(av_lease_guest_begin(&f.guest, &Ops, &f) == -1);
    }
    for (unsigned kind = 0; kind < 6; ++kind) {
        fixture_t f = fixture(); assert(av_lease_guest_begin(&f.guest, &Ops, &f) == 0);
        assert(av_lease_guest_transition(&f.guest, 0, 0, &Ops, &f) == 0);
        av_message_t ack = {.type = MsgInputEpochAck, .lease_generation = 1, .buffer_index = 1};
        if (kind == 0) f.now = f.guest.deadline;
        if (kind == 1) f.fail_reconcile = 1;
        if (kind == 2) f.fail_publish = 1;
        if (kind == 3) f.guest.phase = AvLeaseUnfocusedReady;
        if (kind == 4) f.guest.phase = AvLeaseActive;
        if (kind == 5) { f.now = f.guest.deadline - 1; assert(av_lease_guest_check(&f.guest, f.now) == 0); ++f.now; }
        assert(av_lease_guest_apply(&f.guest, &ack, &Ops, &f) == -1);
    }
    fixture_t f = fixture(); handshake(&f);
    av_message_t ack = {.type = MsgInputEpochAck, .lease_generation = 1, .buffer_index = 2};
    assert(av_lease_guest_apply(&f.guest, &ack, &Ops, &f) == -1);
    f = fixture(); handshake(&f); focus(&f); f.guest.epoch = UINT64_MAX - 1; f.live_id = 8;
    assert(av_lease_guest_transition(&f.guest, 8, 99, &Ops, &f) == 0 && f.guest.epoch == UINT64_MAX);
}
static void host_errors(void) {
    for (unsigned kind = 0; kind < 9; ++kind) {
        av_lease_host_t h = {0};
        av_message_t revoked = {.type = MsgInputEpochRevoked, .lease_generation = 1};
        if (kind == 0) revoked.type = MsgInputEpochAck;
        if (kind == 1) revoked.height = 1;
        if (kind == 2) revoked.lease_generation = 2;
        if (kind == 3) h.epoch = UINT64_MAX;
        if (kind == 4) h.phase = AvLeaseBarrier;
        if (kind == 5) h.phase = AvLeaseAwaitReady;
        if (kind == 6) h.phase = AvLeaseHostTerminal;
        assert(av_lease_host_receive(&h, &revoked, kind == 7 ? UINT64_MAX : kind == 8 ? 0 : 100) == -1);
    }
    av_lease_host_t h = {0}; assert(av_lease_host_receive(&h, NULL, 100) == -1);
    for (unsigned kind = 0; kind < 6; ++kind) {
        fixture_t f = fixture(); handshake(&f);
        av_message_t ready = f.last;
        if (kind == 0) ready.lease_generation = 2;
        if (kind == 1) f.host.phase = AvLeaseAwaitInitialRevocation;
        if (kind == 2) { f.host.phase = AvLeaseAwaitReady; ++ready.buffer_index; }
        if (kind == 3) { f.host.phase = AvLeaseAwaitReady; f.host.deadline = f.now; }
        if (kind == 4) f.host.phase = AvLeaseBarrier;
        assert(av_lease_host_receive(&f.host, &ready, f.now) == -1);
    }
    fixture_t f = fixture(); handshake(&f); f.host.epoch = 2;
    assert(av_lease_host_receive(&f.host, &f.last, f.now) == 0);
    for (unsigned kind = 0; kind < 4; ++kind) {
        h = (av_lease_host_t){.phase = AvLeaseBarrier, .epoch = 1, .deadline = 2100};
        if (kind == 0) h.serial = UINT32_MAX;
        if (kind == 1) h.phase = AvLeaseAwaitReady;
        av_message_t ack;
        assert(av_lease_host_ack(&h, kind == 2 ? 2100 : kind == 3 ? 0 : 100, &ack) == -1);
    }
    h = (av_lease_host_t){.phase = AvLeaseBarrier, .epoch = 1, .deadline = 2100, .serial = UINT32_MAX - 1};
    av_message_t ack; assert(av_lease_host_ack(&h, 100, &ack) == 0 && ack.buffer_index == UINT32_MAX);
    for (unsigned kind = 0; kind < 7; ++kind) {
        h = (av_lease_host_t){.phase = AvLeaseForwarding, .epoch = 1};
        av_message_t event = {.type = MsgInputKeyV3, .window_id = 7, .sequence = 99, .width = 30};
        if (kind == 0) h.phase = AvLeaseBarrier;
        if (kind == 1) h.phase = AvLeaseHostUnfocusedReady;
        if (kind == 2) h.serial = UINT32_MAX;
        if (kind == 3) event.type = MsgInputFocus;
        if (kind == 4) event.type = 24;
        if (kind == 5) event.window_id = 0;
        if (kind == 6) { h.phase = AvLeaseHostUnfocusedReady; event.type = MsgInputFocusV3; }
        assert(av_lease_host_stamp(&h, &event) == -1);
    }
    h = (av_lease_host_t){.phase = AvLeaseForwarding, .epoch = 1, .serial = UINT32_MAX - 1};
    av_message_t event = {.type = MsgInputKeyV3, .window_id = 7, .sequence = 99, .width = 30};
    assert(av_lease_host_stamp(&h, &event) == 0 && event.buffer_index == UINT32_MAX);
}
static void compatibility(void) {
    av_message_t create = {.type = MsgWindowCreateV3, .window_id = 7, .sequence = 99,
        .width = 640, .height = 480, .dpi = 96, .process_id = 55};
    uint8_t bytes[AvControlBytes], old[328]; memset(old, 0xa5, sizeof(old));
    assert(av_control_encode(&create, bytes, sizeof(bytes)) == 0);
    assert(v2_av_control_decode(bytes, sizeof(bytes), old) == -1);
    for (size_t i = 0; i < sizeof(old); ++i) assert(old[i] == 0xa5);
    create.type = MsgWindowCreateV2;
    assert(v2_av_control_encode(&create, bytes, sizeof(bytes)) == 0);
    av_message_t decoded = {0}; assert(av_control_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(!decoded.lease_generation && decoded.sequence == 99);
    for (unsigned kind = 1; kind <= 3; ++kind) {
        av_identity_session_t identity = {0};
        create.type = kind == 1 ? MsgWindowCreate : kind == 2 ? MsgWindowCreateV2 : MsgWindowCreateV3;
        assert(av_identity_admit(&identity, &create) == 0 && identity.mode == (av_identity_mode_t)kind);
        create.type = kind == 3 ? MsgWindowCreateV2 : MsgWindowCreateV3; ++create.sequence;
        assert(av_identity_admit(&identity, &create) == -1);
    }
}
static int guest_receive(const av_message_t *message, void *context) {
    fixture_t *f = context;
    return av_lease_guest_apply(&f->guest, message, &Ops, f);
}
static int host_receive(const av_message_t *message, void *context) {
    fixture_t *f = context;
    return av_lease_host_receive(&f->host, message, f->now) < 0 ? -1 : 0;
}
static void transport(void) {
    fixture_t f = fixture();
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    av_peer_t guest = {.socket = (uintptr_t)sockets[0]}, host = {.socket = (uintptr_t)sockets[1]};
    f.output = &guest;
    assert(av_lease_guest_begin(&f.guest, &Ops, &f) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(av_peer_pump(&host, host_receive, &f) == 0 && f.host.phase == AvLeaseBarrier);
    av_message_t ack;
    assert(av_lease_host_ack(&f.host, f.now, &ack) == 0);
    uint8_t bytes[AvControlBytes]; assert(av_control_encode(&ack, bytes, sizeof(bytes)) == 0);
    for (unsigned i = 0; i < AvControlBytes; ++i) {
        assert(write(sockets[1], bytes + i, 1) == 1);
        assert(av_peer_pump(&guest, guest_receive, &f) == 0);
        assert(f.guest.phase == (i + 1 == AvControlBytes ? AvLeaseUnfocusedReady : AvLeaseAwaitAck));
    }
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(av_peer_pump(&host, host_receive, &f) == 0 && f.host.phase == AvLeaseHostUnfocusedReady);
    for (unsigned i = 0; i < 4; ++i) {
        av_message_t event = input(&f, i == 0 ? MsgInputFocusV3 : i == 1 ? MsgInputPointerV3 :
            i == 2 ? MsgInputKeyV3 : MsgInputButtonV3, i != 1, i == 2 ? 30 : i == 3 ? 1 : 0);
        assert(av_lease_host_stamp(&f.host, &event) == 0);
        f.host.phase = AvLeaseForwarding;
        assert(av_peer_send(&host, &event) == 0);
    }
    assert(av_peer_pump(&host, host_receive, &f) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(f.activations == 1 && f.emitted == 3 && f.guest.input.keys[30] && f.guest.input.buttons[1]);
    uint64_t old_epoch = f.guest.epoch;
    f.live_id = 8;
    assert(av_lease_guest_transition(&f.guest, 8, 99, &Ops, &f) == 0);
    unsigned effects = f.emitted + f.released + f.activations;
    av_message_t stale = input(&f, MsgInputFocusV3, 1, 0); stale.lease_generation = old_epoch;
    stale.window_id = 7; stale.buffer_index = ++f.host.serial;
    assert(av_peer_send(&host, &stale) == 0);
    stale.type = MsgInputKeyV3; stale.width = 30; stale.buffer_index = ++f.host.serial;
    assert(av_peer_send(&host, &stale) == 0);
    assert(av_peer_pump(&host, host_receive, &f) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(effects == f.emitted + f.released + f.activations);
    assert(av_peer_pump(&host, host_receive, &f) == 0 && f.host.phase == AvLeaseBarrier);
    assert(av_lease_host_ack(&f.host, f.now, &ack) == 0 && ack.buffer_index > AvVideoBuffers);
    assert(av_peer_send(&host, &ack) == 0 && av_peer_pump(&host, host_receive, &f) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == 0);
    assert(av_peer_pump(&host, host_receive, &f) == 0);
    assert(!f.guest.input.window_id && !f.guest.input.keys[30] && !f.guest.input.buttons[1]);
    /* Disconnect mid-frame is terminal and never dispatches an incomplete focus. */
    av_message_t event = input(&f, MsgInputFocusV3, 1, 0);
    assert(av_control_encode(&event, bytes, sizeof(bytes)) == 0);
    assert(write(sockets[1], bytes, 3) == 3); assert(close(sockets[1]) == 0);
    assert(av_peer_pump(&guest, guest_receive, &f) == -1 && guest.failed);
    assert(close(sockets[0]) == 0);
    assert(av_input_reset(&f.guest.input, &Ops.input, &f) == 0);
    /* Full required-control queues terminate even after a successful Create. */
    f = fixture(); guest = (av_peer_t){.head = AvPeerQueueFrames - 1}; f.output = &guest;
    av_message_t create = {.type = MsgWindowCreateV3, .window_id = 7, .sequence = 99,
        .width = 640, .height = 480, .dpi = 96, .process_id = 55};
    assert(av_peer_send(&guest, &create) == 0);
    assert(av_lease_guest_begin(&f.guest, &Ops, &f) == -1 && guest.failed);
    assert(f.guest.epoch == 1 && f.guest.phase == AvLeaseGuestTerminal);
    /* A fresh peer and zero-initialized lease must repeat the entire handshake. */
    f = fixture(); handshake(&f); assert(f.guest.input.serial == 1 && !f.guest.ever_anchored);
}
int main(void) {
    startup_and_ordering(); stale_and_invalid(); releases_and_transition();
    deadlines_and_handshakes(); host_errors(); compatibility(); transport();
    puts("AV lease: authority, ordering, revocation, exact handshake, deadlines and V2 compatibility passed");
    return 0;
}
