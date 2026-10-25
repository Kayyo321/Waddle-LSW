/**
 * @file control_peer_abi.c
 * @brief Real C round-trip and runtime guard tests for the opaque control-peer ABI.
 *
 * @ownership All buffers and peer slots belong to this test. Peers are destroyed
 * before process exit; test frames and copied TX/RX bytes use stack storage.
 * @thread_safety The fixture is single-threaded, matching the ABI serialization contract.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waddle/interop_peer.h"
#include "waddle/interop_protocol.h"

_Static_assert(sizeof(interop_peer_config_t) == 48, "config ABI size");
_Static_assert(_Alignof(interop_peer_config_t) == 8, "config ABI alignment");
_Static_assert(offsetof(interop_peer_config_t, role) == 0, "config.role offset");
_Static_assert(offsetof(interop_peer_config_t, reserved_zero) == 4, "config.reserved offset");
_Static_assert(offsetof(interop_peer_config_t, connection_token) == 8, "config.token offset");
_Static_assert(offsetof(interop_peer_config_t, accepted_at_ms) == 16, "config.time offset");
_Static_assert(offsetof(interop_peer_config_t, agent_version) == 24, "config.version offset");
_Static_assert(offsetof(interop_peer_config_t, agent_version_bytes) == 32, "config.version length offset");
_Static_assert(offsetof(interop_peer_config_t, local_capabilities) == 40, "config.capabilities offset");
_Static_assert(sizeof(interop_peer_event_t) == 56, "event ABI size");
_Static_assert(_Alignof(interop_peer_event_t) == 8, "event ABI alignment");
_Static_assert(offsetof(interop_peer_event_t, kind) == 0, "event.kind offset");
_Static_assert(offsetof(interop_peer_event_t, phase) == 4, "event.phase offset");
_Static_assert(offsetof(interop_peer_event_t, flags) == 8, "event.flags offset");
_Static_assert(offsetof(interop_peer_event_t, error_code) == 12, "event.error offset");
_Static_assert(offsetof(interop_peer_event_t, session_epoch) == 16, "event.epoch offset");
_Static_assert(offsetof(interop_peer_event_t, capabilities) == 24, "event.capabilities offset");
_Static_assert(offsetof(interop_peer_event_t, layout_version) == 32, "event.layout offset");
_Static_assert(offsetof(interop_peer_event_t, message_type) == 36, "event.type offset");
_Static_assert(offsetof(interop_peer_event_t, request_id) == 40, "event.request offset");
_Static_assert(offsetof(interop_peer_event_t, reserved_zero) == 44, "event.reserved offset");
_Static_assert(offsetof(interop_peer_event_t, copied_bytes) == 48, "event.copied offset");

enum { HostToken = 0x101, GuestToken = 0x202, Epoch = 0x334455, MaxFrame = 512 };
static const uint64_t Capabilities = InteropCapWindows | InteropCapInput;
static const char Version[] = "1.2.3";

static void fail_at(const char *expression, int line)
{
    fprintf(stderr, "control_peer_abi.c:%d: check failed: %s\n", line, expression);
    exit(1);
}

#define Check(expression) do { if (!(expression)) fail_at(#expression, __LINE__); } while (0)

static interop_peer_config_t peer_config(uint32_t role, uint64_t token, uint64_t capabilities)
{
    interop_peer_config_t config = {
        .role = role,
        .reserved_zero = 0,
        .connection_token = token,
        .accepted_at_ms = 100,
        .agent_version = (const uint8_t *)Version,
        .agent_version_bytes = sizeof(Version) - 1,
        .local_capabilities = capabilities,
    };
    return config;
}

static void expect_event(const interop_peer_event_t *event, uint32_t kind, uint32_t phase)
{
    if (event->kind != kind || event->phase != phase)
        fprintf(stderr, "event got kind=%u phase=%u, expected kind=%u phase=%u\n", event->kind, event->phase, kind, phase);
    Check(event->kind == kind);
    Check(event->phase == phase);
    Check(event->reserved_zero == 0);
}

static size_t make_frame(uint8_t *frame, size_t capacity, uint16_t type, uint32_t request_id,
                         interop_direction_t direction, int catalog)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    if (catalog) {
        Check(interop_writer_finish(&writer, type, request_id, direction, &frame_length) == InteropErrorNone);
    } else {
        Check(type == InteropMsgPing || type == InteropMsgPong);
        Check(interop_writer_finish(&writer, type, request_id, direction, &frame_length) == InteropErrorNone);
    }
    return frame_length;
}

static void feed_all(interop_peer_t *peer, uint64_t token, const uint8_t *bytes, size_t length,
                     size_t fragment, interop_peer_event_t *event)
{
    size_t offset = 0;
    while (offset < length) {
        size_t consumed = SIZE_MAX;
        size_t count = length - offset < fragment ? length - offset : fragment;
        Check(interop_peer_feed(peer, token, bytes + offset, count, &consumed, event) == InteropPeerSuccess);
        Check(consumed == count);
        offset += consumed;
    }
}

static size_t copy_tx(interop_peer_t *peer, uint64_t token, uint8_t *out, size_t capacity)
{
    size_t length = SIZE_MAX;
    Check(interop_peer_copy_tx(peer, token, out, capacity, &length) == InteropPeerSuccess);
    return length;
}

static void send_front(interop_peer_t *sender, uint64_t sender_token, interop_peer_t *receiver,
                      uint64_t receiver_token, uint64_t now_ms, interop_peer_event_t *sender_event)
{
    uint8_t frame[MaxFrame];
    size_t available = 0;
    size_t copied = 0;
    size_t offset = 0;
    interop_header_t header;
    Check(interop_peer_tx_bytes(sender, sender_token, &available) == InteropPeerSuccess);
    Check(available > 0 && available <= sizeof(frame));
    copied = copy_tx(sender, sender_token, frame, sizeof(frame));
    Check(copied == available);
    Check(interop_header_decode(frame, copied, &header) == InteropErrorNone);
    while (offset < copied) {
        size_t count = copied - offset > 7 ? 7 : copied - offset;
        size_t consumed = SIZE_MAX;
        Check(interop_peer_feed(receiver, receiver_token, frame + offset, count, &consumed, sender_event) == InteropPeerSuccess);
        Check(consumed == count);
        Check(interop_peer_ack_tx(sender, sender_token, now_ms, count, sender_event) == InteropPeerSuccess);
        offset += count;
    }
}

static void test_invalid_create_and_runtime_pointer_guards(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *owner = NULL;
    interop_peer_t *sentinel_owner = (interop_peer_t *)(uintptr_t)0x1234;
    interop_peer_t *invalid_owner = (interop_peer_t *)(uintptr_t)1;
    _Alignas(8) char misaligned_config[sizeof(config) + 1];
    _Alignas(8) char misaligned_out[sizeof(owner) + 1];
    volatile uintptr_t config_address = (uintptr_t)&config;
    volatile uintptr_t out_address = (uintptr_t)&owner;
    volatile uintptr_t overflow_address = UINTPTR_MAX - 7;
    config.role = 99;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.reserved_zero = 1;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, 0, Capabilities);
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.agent_version = NULL;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.agent_version_bytes = 4;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.agent_version = (const uint8_t *)"01.2.3";
    config.agent_version_bytes = sizeof("01.2.3") - 1;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.local_capabilities = UINT64_C(1) << 63;
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    config.agent_version = (const uint8_t *)&config;
    config.agent_version_bytes = sizeof(config);
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == NULL);

    owner = sentinel_owner;
    config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    Check(interop_peer_create(&config, &owner) == InteropPeerInvalidArgument && owner == sentinel_owner);
    owner = NULL;
    memcpy(misaligned_config + 1, &config, sizeof(config));
    memcpy(misaligned_out + 1, &owner, sizeof(owner));
    Check(interop_peer_create((const interop_peer_config_t *)(const void *)(misaligned_config + 1), &owner) == InteropPeerInvalidArgument);
    Check(interop_peer_create(&config, (interop_peer_t **)(void *)(misaligned_out + 1)) == InteropPeerInvalidArgument);
    Check(interop_peer_create((const interop_peer_config_t *)(uintptr_t)1, &owner) == InteropPeerInvalidArgument);
    Check(interop_peer_create(&config, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_create((const interop_peer_config_t *)(uintptr_t)config_address,
                              (interop_peer_t **)(uintptr_t)config_address) == InteropPeerInvalidArgument);
    Check(interop_peer_create((const interop_peer_config_t *)config_address,
                              (interop_peer_t **)overflow_address) == InteropPeerInvalidArgument);
    Check(interop_peer_destroy(&invalid_owner) == InteropPeerInvalidArgument && invalid_owner == (interop_peer_t *)(uintptr_t)1);
    Check(owner == NULL);
    (void)out_address;
}

static void test_null_owner_is_rejected_by_every_operation(void)
{
    interop_peer_event_t event;
    size_t length = SIZE_MAX;
    size_t consumed = SIZE_MAX;
    uint8_t byte = 0;

    memset(&event, 0xA5, sizeof(event));
    Check(interop_peer_destroy(NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_get_state(NULL, GuestToken, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(NULL, GuestToken, &byte, 1, &consumed, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_pending_bytes(NULL, GuestToken, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_next_event(NULL, GuestToken, 100, &byte, 1, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_mapping_result(NULL, GuestToken, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_tx_bytes(NULL, GuestToken, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_copy_tx(NULL, GuestToken, &byte, 1, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_ack_tx(NULL, GuestToken, 100, 0, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_tick(NULL, GuestToken, 100, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_begin_drain(NULL, GuestToken, 100, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_transport_lost(NULL, GuestToken, &event) == InteropPeerInvalidArgument);
}

static void test_zero_token_is_rejected_by_every_peer_operation(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    size_t length = SIZE_MAX;
    size_t consumed = SIZE_MAX;
    uint8_t byte = 0;

    Check(interop_peer_create(&config, &guest) == InteropPeerSuccess);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_get_state(guest, 0, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(guest, 0, &byte, 1, &consumed, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_pending_bytes(guest, 0, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_next_event(guest, 0, 100, NULL, 0, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_mapping_result(guest, 0, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_tx_bytes(guest, 0, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_copy_tx(guest, 0, NULL, 0, &length) == InteropPeerInvalidArgument);
    Check(interop_peer_ack_tx(guest, 0, 100, 0, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_tick(guest, 0, 100, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_begin_drain(guest, 0, 100, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_transport_lost(guest, 0, &event) == InteropPeerInvalidArgument);
    Check(consumed == SIZE_MAX && length == SIZE_MAX && memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess && guest == NULL);
}

static void test_malformed_header_is_reported_as_terminal_feed_result(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    uint8_t malformed[InteropHeaderBytes] = { 0 };
    size_t consumed = SIZE_MAX;

    Check(interop_peer_create(&config, &guest) == InteropPeerSuccess);
    Check(interop_peer_feed(guest, GuestToken, malformed, sizeof(malformed), &consumed, &event) == InteropPeerSuccess);
    Check(consumed == sizeof(malformed));
    expect_event(&event, InteropPeerEventDraining, InteropPeerDraining);
    Check(event.error_code != InteropErrorNone);
    Check(interop_peer_get_state(guest, GuestToken, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerDraining);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess && guest == NULL);
}

static void test_operation_outputs_tokens_and_callback_failures(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    size_t length = SIZE_MAX;
    size_t consumed = SIZE_MAX;
    uint8_t byte = 0;

    Check(interop_peer_create(&config, &guest) == InteropPeerSuccess);
    Check(interop_peer_next_event(guest, GuestToken, 100, NULL, 0, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerInitializingMapping);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_get_state(guest, GuestToken + 1, &event) == InteropPeerWrongToken);
    Check(interop_peer_feed(guest, GuestToken + 1, &byte, 1, &consumed, &event) == InteropPeerWrongToken);
    Check(consumed == SIZE_MAX && memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_pending_bytes(guest, GuestToken + 1, &length) == InteropPeerWrongToken);
    Check(length == SIZE_MAX);
    Check(interop_peer_next_event(guest, GuestToken + 1, 100, NULL, 0, &event) == InteropPeerWrongToken);
    Check(interop_peer_mapping_result(guest, GuestToken + 1, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerWrongToken);
    Check(interop_peer_tx_bytes(guest, GuestToken + 1, &length) == InteropPeerWrongToken);
    Check(interop_peer_copy_tx(guest, GuestToken + 1, NULL, 0, &length) == InteropPeerWrongToken);
    Check(interop_peer_ack_tx(guest, GuestToken + 1, 100, 0, &event) == InteropPeerWrongToken);
    Check(interop_peer_tick(guest, GuestToken + 1, 100, &event) == InteropPeerWrongToken);
    Check(interop_peer_begin_drain(guest, GuestToken + 1, 100, &event) == InteropPeerWrongToken);
    Check(interop_peer_transport_lost(guest, GuestToken + 1, &event) == InteropPeerWrongToken);
    Check(memcmp(&event, &before, sizeof(event)) == 0);

    Check(interop_peer_get_state(guest, GuestToken, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(guest, GuestToken, &byte, 1, NULL, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(guest, GuestToken, &byte, 1, &consumed, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_pending_bytes(guest, GuestToken, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_next_event(guest, GuestToken, 100, NULL, 0, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_tx_bytes(guest, GuestToken, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_copy_tx(guest, GuestToken, NULL, 0, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_ack_tx(guest, GuestToken, 100, 0, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_tick(guest, GuestToken, 100, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_begin_drain(guest, GuestToken, 100, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_transport_lost(guest, GuestToken, NULL) == InteropPeerInvalidArgument);

    /* Aligned in-range outputs that overlap the live owner must be rejected
     * after all preceding inputs have passed validation. */
    Check(interop_peer_get_state(guest, GuestToken, (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(guest, GuestToken, &byte, 1, &consumed,
                            (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_pending_bytes(guest, GuestToken, (size_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_next_event(guest, GuestToken, 100, NULL, 0,
                                  (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1,
                                      (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_tx_bytes(guest, GuestToken, (size_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_copy_tx(guest, GuestToken, NULL, 0, (size_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_ack_tx(guest, GuestToken, 100, 0,
                              (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_tick(guest, GuestToken, 100,
                            (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_begin_drain(guest, GuestToken, 100,
                                   (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_transport_lost(guest, GuestToken,
                                      (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);

    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 0, &event) == InteropPeerMappingFailed);
    Check(interop_peer_get_state(guest, GuestToken, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerInitializingMapping);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess);
}

static void test_timer_and_empty_queue_error_paths(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    uint8_t byte = 0;
    size_t consumed = SIZE_MAX;
    size_t length = SIZE_MAX;
    size_t copied = SIZE_MAX;

    Check(interop_peer_create(&config, &guest) == InteropPeerSuccess);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_next_event(guest, GuestToken, 99, &byte, sizeof(byte), &event) == InteropPeerTimeWentBackwards);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_mapping_result(guest, GuestToken, 99, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerTimeWentBackwards);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_ack_tx(guest, GuestToken, 100, 0, &event) == InteropPeerNoQueuedFrame);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tick(guest, GuestToken, 100, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerInitializingMapping);

    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    Check(interop_peer_ack_tx(guest, GuestToken, 99, 0, &event) == InteropPeerTimeWentBackwards);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length > 0);
    Check(interop_peer_copy_tx(guest, GuestToken, (uint8_t *)(void *)guest, length, &copied) == InteropPeerInvalidArgument);
    Check(interop_peer_feed(guest, GuestToken, &byte, 1, &consumed, &event) == InteropPeerSuccess);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess);
}

static void test_roundtrip_and_feature_copy(void)
{
    interop_peer_config_t host_config = peer_config(InteropPeerHost, HostToken, Capabilities);
    interop_peer_config_t guest_config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *host = NULL;
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    uint8_t tx[MaxFrame];
    uint8_t output[MaxFrame];
    _Alignas(8) uint8_t catalog[MaxFrame];
    size_t length = 0;
    size_t consumed = 0;
    size_t frame_length = 0;
    size_t pending = 0;
    size_t copied = 0;
    size_t offset;

    Check(interop_peer_create(&host_config, &host) == InteropPeerSuccess && host != NULL);
    Check(interop_peer_create(&guest_config, &guest) == InteropPeerSuccess && guest != NULL);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_get_state(guest, 0, &event) == InteropPeerInvalidArgument);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_get_state(guest, GuestToken, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerInitializingMapping);

    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerAwaitHelloSent);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length > InteropHeaderBytes);
    memset(tx, 0xCC, sizeof(tx));
    copied = SIZE_MAX;
    Check(interop_peer_copy_tx(guest, GuestToken, tx, length - 1, &copied) == InteropPeerOutputTooSmall);
    Check(copied == SIZE_MAX && tx[0] == 0xCC && tx[length - 1] == 0xCC);
    Check(interop_peer_copy_tx(guest, GuestToken, tx, sizeof(tx), &copied) == InteropPeerSuccess && copied == length);
    {
        interop_header_t header;
        Check(interop_header_decode(tx, copied, &header) == InteropErrorNone);
        Check(header.type == InteropMsgHello && header.request_id != 0);
    }

    consumed = SIZE_MAX;
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_feed(host, HostToken + 1, tx, 3, &consumed, &event) == InteropPeerWrongToken);
    Check(consumed == SIZE_MAX && memcmp(&event, &before, sizeof(event)) == 0);
    consumed = 0;
    offset = 0;
    while (offset < copied) {
        size_t count = copied - offset > 3 ? 3 : copied - offset;
        Check(interop_peer_feed(host, HostToken, tx + offset, count, &consumed, &event) == InteropPeerSuccess);
        Check(consumed == count);
        Check(interop_peer_ack_tx(guest, GuestToken, 100, count, &event) == InteropPeerSuccess);
        offset += count;
        if (offset < copied) Check(event.phase == InteropPeerAwaitHelloSent);
    }
    Check(event.phase == InteropPeerAwaitHelloAck);
    Check(interop_peer_pending_bytes(host, HostToken, &pending) == InteropPeerSuccess && pending == copied);

    memset(output, 0x5A, sizeof(output));
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_next_event(host, HostToken, 100, output, sizeof(output), &event) == InteropPeerSuccess);
    Check(interop_peer_pending_bytes(host, HostToken, &pending) == InteropPeerSuccess && pending == 0);
    expect_event(&event, InteropPeerEventValidateMapping, InteropPeerAwaitMapping);
    Check(event.session_epoch == Epoch && event.layout_version == InteropLayoutVersion);

    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_mapping_result(host, HostToken, 101, Epoch, InteropLayoutVersion, 2, &event) == InteropPeerInvalidArgument);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_mapping_result(host, HostToken, 101, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerAwaitHelloAckSent);

    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length > InteropHeaderBytes);
    Check(interop_peer_copy_tx(host, HostToken, tx, sizeof(tx), &copied) == InteropPeerSuccess && copied == length);
    {
        interop_header_t header;
        Check(interop_header_decode(tx, copied, &header) == InteropErrorNone);
        Check(header.type == InteropMsgHelloAck && header.request_id != 0);
    }
    offset = 0;
    while (offset < copied) {
        size_t count = copied - offset > 5 ? 5 : copied - offset;
        Check(interop_peer_feed(guest, GuestToken, tx + offset, count, &consumed, &event) == InteropPeerSuccess && consumed == count);
        Check(interop_peer_ack_tx(host, HostToken, 101, count, &event) == InteropPeerSuccess);
        offset += count;
        if (offset < copied) Check(event.phase == InteropPeerAwaitHelloAckSent);
    }
    expect_event(&event, InteropPeerEventReady, InteropPeerReady);
    Check(interop_peer_pending_bytes(guest, GuestToken, &pending) == InteropPeerSuccess && pending == copied);
    Check(interop_peer_next_event(guest, GuestToken, 101, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventReady, InteropPeerReady);
    Check(event.session_epoch == Epoch && event.capabilities == Capabilities);

    /* Ping/Pong are real codec frames and are copied/consumed by both C peers. */
    Check(interop_peer_tick(host, HostToken, 2101, &event) == InteropPeerSuccess);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length == InteropHeaderBytes);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_tick(host, HostToken + 1, 2101, &event) == InteropPeerWrongToken);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tick(host, HostToken, 2100, &event) == InteropPeerTimeWentBackwards);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_ack_tx(host, HostToken, 2100, 0, &event) == InteropPeerTimeWentBackwards);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_ack_tx(host, HostToken, 2101, length + 1, &event) == InteropPeerInvalidWriteCount);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tx_bytes(host, HostToken, &pending) == InteropPeerSuccess && pending == length);
    send_front(host, HostToken, guest, GuestToken, 2101, &event);
    Check(interop_peer_next_event(guest, GuestToken, 2101, output, sizeof(output), &event) == InteropPeerSuccess);
    Check(event.kind == InteropPeerEventNone && event.phase == InteropPeerReady);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length == InteropHeaderBytes);
    send_front(guest, GuestToken, host, HostToken, 2101, &event);
    Check(interop_peer_next_event(host, HostToken, 2101, output, sizeof(output), &event) == InteropPeerSuccess);
    Check(event.kind == InteropPeerEventNone && event.phase == InteropPeerReady);

    frame_length = make_frame(catalog, sizeof(catalog), InteropMsgCatalogBegin, 0, InteropFromGuest, 1);
    {
        uint8_t catalog_copy[MaxFrame];
        size_t alias_consumed = SIZE_MAX;
        memcpy(catalog_copy, catalog, frame_length);
        Check(interop_peer_feed(host, HostToken, catalog, frame_length, (size_t *)(void *)catalog,
                                &event) == InteropPeerInvalidArgument);
        Check(memcmp(catalog, catalog_copy, frame_length) == 0);
        Check(interop_peer_feed(host, HostToken, catalog, frame_length, &alias_consumed,
                                (interop_peer_event_t *)(void *)catalog) == InteropPeerInvalidArgument);
        Check(memcmp(catalog, catalog_copy, frame_length) == 0);
        Check(interop_peer_pending_bytes(host, HostToken, &pending) == InteropPeerSuccess && pending == 0);
        Check(interop_peer_feed(host, HostToken, (const uint8_t *)(const void *)host, 1,
                                &alias_consumed, &event) == InteropPeerInvalidArgument);
        _Alignas(8) uint8_t overlapping[sizeof(interop_peer_event_t) + 16];
        memset(overlapping, 0x6D, sizeof(overlapping));
        Check(interop_peer_feed(host, HostToken, catalog, frame_length, (size_t *)(void *)overlapping,
                                (interop_peer_event_t *)(void *)overlapping) == InteropPeerInvalidArgument);
        Check(overlapping[0] == 0x6D);
    }
    feed_all(host, HostToken, catalog, frame_length, 2, &event);
    memset(output, 0x5A, sizeof(output));
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_next_event(host, HostToken, 2102, (uint8_t *)(void *)host,
                                  frame_length, &event) == InteropPeerInvalidArgument);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_next_event(host, HostToken, 2102, output, frame_length - 1, &event) == InteropPeerOutputTooSmall);
    Check(output[0] == 0x5A && memcmp(&event, &before, sizeof(event)) == 0);
    {
        _Alignas(8) uint8_t overlapping[sizeof(interop_peer_event_t) + MaxFrame];
        memset(overlapping, 0x39, sizeof(overlapping));
        Check(interop_peer_next_event(host, HostToken, 2102, overlapping, frame_length,
                                      (interop_peer_event_t *)(void *)overlapping) == InteropPeerInvalidArgument);
        Check(overlapping[0] == 0x39);
    }
    Check(interop_peer_pending_bytes(host, HostToken, &pending) == InteropPeerSuccess && pending == frame_length);
    Check(interop_peer_next_event(host, HostToken, 2101, output, sizeof(output), &event) == InteropPeerSuccess);
    Check(event.kind == InteropPeerEventFeatureFrame && event.message_type == InteropMsgCatalogBegin);
    Check(event.copied_bytes == frame_length && memcmp(output, catalog, frame_length) == 0);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess && guest == NULL);
    Check(interop_peer_destroy(&host) == InteropPeerSuccess && host == NULL);
}

static void test_local_errors_drain_abort_and_closed_state(void)
{
    interop_peer_config_t config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    size_t length = 0x777;
    _Alignas(8) uint8_t overlapping[MaxFrame];
    uint8_t output[MaxFrame];
    uint8_t tx[MaxFrame];
    size_t copied = 0;
    size_t consumed = 0x888;
    uint8_t bad_header[InteropHeaderBytes] = { 0 };
    volatile uintptr_t event_address;
    volatile uintptr_t overflow_input_address = UINTPTR_MAX - 7;

    Check(interop_peer_create(&config, &guest) == InteropPeerSuccess);
    Check(interop_peer_get_state(guest, GuestToken, (interop_peer_event_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_pending_bytes(guest, GuestToken, (size_t *)(void *)guest) == InteropPeerInvalidArgument);
    Check(interop_peer_tx_bytes(guest, GuestToken, NULL) == InteropPeerInvalidArgument);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_mapping_result(guest, GuestToken, 101, Epoch, InteropLayoutVersion + 1, 1, &event) == InteropPeerInvalidArgument);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_mapping_result(guest, GuestToken, 101, Epoch, InteropLayoutVersion, 0, &event) == InteropPeerMappingFailed);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length == 0);
    Check(interop_peer_pending_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length == 0);
    Check(interop_peer_copy_tx(guest, GuestToken, NULL, 0, &length) == InteropPeerSuccess && length == 0);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_feed(guest, GuestToken, NULL, 0, &consumed, &event) == InteropPeerSuccess);
    Check(consumed == 0 && event.kind == InteropPeerEventNone);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    event_address = (uintptr_t)&event;
    Check(interop_peer_get_state(guest, GuestToken, (interop_peer_event_t *)(uintptr_t)(event_address + 1)) == InteropPeerInvalidArgument);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_feed(guest, GuestToken, (const uint8_t *)overflow_input_address, 16,
                            &consumed, &event) == InteropPeerInvalidArgument);
    Check(consumed == 0 && memcmp(&event, &before, sizeof(event)) == 0);

    Check(interop_peer_mapping_result(guest, GuestToken, 101, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerAwaitHelloSent);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length > 0);
    memset(overlapping, 0xAB, sizeof(overlapping));
    Check(interop_peer_copy_tx(guest, GuestToken, overlapping, length,
                               (size_t *)(void *)overlapping) == InteropPeerInvalidArgument);
    Check(overlapping[0] == 0xAB);
    {
        _Alignas(8) uint8_t overlapping[MaxFrame];
        memset(overlapping, 0xAB, sizeof(overlapping));
        Check(interop_peer_copy_tx(guest, GuestToken, overlapping, length,
                                   (size_t *)(void *)overlapping) == InteropPeerInvalidArgument);
        Check(overlapping[0] == 0xAB);
    }
    Check(interop_peer_begin_drain(guest, GuestToken, 102, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerDraining && (event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerSuccess && length == 0);
    memset(tx, 0xEE, sizeof(tx));
    Check(interop_peer_copy_tx(guest, GuestToken, tx, sizeof(tx), &copied) == InteropPeerSuccess && copied == 0);
    Check(tx[0] == 0xEE);
    Check(interop_peer_ack_tx(guest, GuestToken, 102, 0, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerDraining && (event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_transport_lost(guest, GuestToken, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerTransportClosed);
    Check((event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_get_state(guest, GuestToken, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerTransportClosed);
    Check((event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_pending_bytes(guest, GuestToken, &length) == InteropPeerSuccess);
    Check(interop_peer_tx_bytes(guest, GuestToken, &length) == InteropPeerWrongPhase);
    Check(interop_peer_copy_tx(guest, GuestToken, output, sizeof(output), &copied) == InteropPeerWrongPhase);
    Check(interop_peer_ack_tx(guest, GuestToken, 102, 0, &event) == InteropPeerWrongPhase);
    Check(interop_peer_tick(guest, GuestToken, 102, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerTransportClosed);
    Check(interop_peer_feed(guest, GuestToken, bad_header, sizeof(bad_header), &consumed, &event) == InteropPeerWrongPhase);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess && guest == NULL);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess && guest == NULL);
    Check(interop_peer_destroy(NULL) == InteropPeerInvalidArgument);
}

static void test_host_mapping_rejection_drains_without_readiness(void)
{
    interop_peer_config_t host_config = peer_config(InteropPeerHost, HostToken, Capabilities);
    interop_peer_config_t guest_config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *host = NULL;
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    uint8_t hello[MaxFrame];
    uint8_t output[MaxFrame];
    size_t hello_length = 0;
    size_t copied = 0;
    size_t consumed = 0;

    Check(interop_peer_create(&host_config, &host) == InteropPeerSuccess);
    Check(interop_peer_create(&guest_config, &guest) == InteropPeerSuccess);
    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    hello_length = copy_tx(guest, GuestToken, hello, sizeof(hello));
    Check(hello_length > InteropHeaderBytes);
    feed_all(host, HostToken, hello, hello_length, hello_length, &event);
    Check(interop_peer_ack_tx(guest, GuestToken, 100, hello_length, &event) == InteropPeerSuccess);
    Check(interop_peer_next_event(host, HostToken, 100, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventValidateMapping, InteropPeerAwaitMapping);
    Check(interop_peer_mapping_result(host, HostToken, 101, Epoch + 1, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventDraining, InteropPeerDraining);
    Check(interop_peer_tx_bytes(host, HostToken, &copied) == InteropPeerSuccess && copied > 0);
    Check(interop_peer_next_event(host, HostToken, 101, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerDraining);
    Check(interop_peer_pending_bytes(host, HostToken, &copied) == InteropPeerSuccess && copied == 0);
    consumed = SIZE_MAX;
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_feed(host, HostToken, NULL, 0, &consumed, &event) == InteropPeerWrongPhase);
    Check(consumed == SIZE_MAX && memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess);
    Check(interop_peer_destroy(&host) == InteropPeerSuccess);
}

static void test_drain_aborts_unfinished_ack(void)
{
    interop_peer_config_t host_config = peer_config(InteropPeerHost, HostToken, Capabilities);
    interop_peer_config_t guest_config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *host = NULL;
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    uint8_t hello[MaxFrame];
    uint8_t output[MaxFrame];
    size_t hello_length;
    size_t length = 0;

    Check(interop_peer_create(&host_config, &host) == InteropPeerSuccess);
    Check(interop_peer_create(&guest_config, &guest) == InteropPeerSuccess);
    Check(interop_peer_mapping_result(guest, GuestToken, 100, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    hello_length = copy_tx(guest, GuestToken, hello, sizeof(hello));
    feed_all(host, HostToken, hello, hello_length, 4, &event);
    Check(interop_peer_ack_tx(guest, GuestToken, 100, hello_length, &event) == InteropPeerSuccess);
    Check(interop_peer_next_event(host, HostToken, 100, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventValidateMapping, InteropPeerAwaitMapping);
    Check(interop_peer_mapping_result(host, HostToken, 101, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length > 0);
    Check(interop_peer_begin_drain(host, HostToken, 102, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerDraining && (event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length == 0);
    Check(interop_peer_ack_tx(host, HostToken, 102, 0, &event) == InteropPeerSuccess);
    Check((event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_transport_lost(host, HostToken, &event) == InteropPeerSuccess);
    Check(event.phase == InteropPeerTransportClosed && (event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_peer_destroy(&guest) == InteropPeerSuccess);
    Check(interop_peer_destroy(&host) == InteropPeerSuccess);
}

static size_t make_window_close(uint8_t *frame, size_t capacity, uint64_t window, uint64_t generation)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, InteropTagWindow, window) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagGeneration, generation) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgWindowClose, 0, InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_clipboard_clear(uint8_t *frame, size_t capacity, interop_direction_t direction, uint64_t epoch)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, InteropTagClipboardEpoch, epoch) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgClipboardClear, 0, direction, &frame_length) == InteropErrorNone);
    return frame_length;
}

static void establish_ready(interop_peer_t *host, interop_peer_t *guest, uint64_t now_ms)
{
    interop_peer_event_t event;
    uint8_t output[MaxFrame];
    Check(interop_peer_mapping_result(guest, GuestToken, now_ms, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerAwaitHelloSent);
    send_front(guest, GuestToken, host, HostToken, now_ms, &event);
    expect_event(&event, InteropPeerEventNone, InteropPeerAwaitHelloAck);
    Check(interop_peer_next_event(host, HostToken, now_ms, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventValidateMapping, InteropPeerAwaitMapping);
    Check(interop_peer_mapping_result(host, HostToken, now_ms, Epoch, InteropLayoutVersion, 1, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerAwaitHelloAckSent);
    send_front(host, HostToken, guest, GuestToken, now_ms, &event);
    expect_event(&event, InteropPeerEventReady, InteropPeerReady);
    Check(interop_peer_next_event(guest, GuestToken, now_ms, output, sizeof(output), &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventReady, InteropPeerReady);
}

static void test_outbound_feature_admission(void)
{
    interop_peer_config_t host_config = peer_config(InteropPeerHost, HostToken, Capabilities);
    interop_peer_config_t guest_config = peer_config(InteropPeerGuest, GuestToken, Capabilities);
    interop_peer_t *host = NULL;
    interop_peer_t *guest = NULL;
    interop_peer_event_t event;
    interop_peer_event_t before;
    _Alignas(8) uint8_t frame[InteropHeaderBytes + 32];
    uint8_t tx[MaxFrame];
    size_t frame_length = 0;
    size_t length = 0;
    size_t index;

    Check(interop_peer_create(&host_config, &host) == InteropPeerSuccess && host != NULL);
    Check(interop_peer_create(&guest_config, &guest) == InteropPeerSuccess && guest != NULL);
    establish_ready(host, guest, 100);

    frame_length = make_window_close(frame, sizeof(frame), 0x55, 2);
    Check(frame_length == 40);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_enqueue_feature(host, HostToken, 101, NULL, 0, &event) == InteropPeerInvalidFrame);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, NULL, frame_length, &event) == InteropPeerInvalidArgument);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, NULL) == InteropPeerInvalidArgument);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, (interop_peer_event_t *)(void *)frame) == InteropPeerInvalidArgument);
    Check(interop_peer_enqueue_feature(host, GuestToken, 101, frame, frame_length, &event) == InteropPeerWrongToken);
    Check(interop_peer_enqueue_feature(host, HostToken, 99, frame, frame_length, &event) == InteropPeerTimeWentBackwards);
    Check(memcmp(&event, &before, sizeof(event)) == 0);

    frame[0] ^= 0xFF;
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerInvalidFrame);
    frame[0] ^= 0xFF;
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length - 4, &event) == InteropPeerInvalidFrame);
    Check(memcmp(&event, &before, sizeof(event)) == 0);

    frame_length = make_frame(frame, sizeof(frame), InteropMsgPing, 1, InteropFromHost, 0);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerUnsupportedFrame);
    frame_length = make_frame(frame, sizeof(frame), InteropMsgCatalogBegin, 0, InteropFromGuest, 1);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerUnsupportedFrame);
    frame_length = make_clipboard_clear(frame, sizeof(frame), InteropFromHost, 3);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerFeatureNotNegotiated);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length == 0);

    frame_length = make_window_close(frame, sizeof(frame), 0x55, 2);
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerReady);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length == frame_length);
    Check(copy_tx(host, HostToken, tx, sizeof(tx)) == frame_length && memcmp(tx, frame, frame_length) == 0);

    for (index = 1; index < 504; index++) {
        frame_length = make_window_close(frame, sizeof(frame), index + 1, 1);
        Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerSuccess);
    }
    frame_length = make_window_close(frame, sizeof(frame), 9999, 3);
    memset(&event, 0xA5, sizeof(event));
    before = event;
    Check(interop_peer_enqueue_feature(host, HostToken, 101, frame, frame_length, &event) == InteropPeerQueueFull);
    Check(memcmp(&event, &before, sizeof(event)) == 0);
    Check(interop_peer_tx_bytes(host, HostToken, &length) == InteropPeerSuccess && length == 40);
    Check(interop_peer_ack_tx(host, HostToken, 102, length, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventNone, InteropPeerReady);
    Check(interop_peer_enqueue_feature(host, HostToken, 102, frame, frame_length, &event) == InteropPeerSuccess);

    Check(interop_peer_enqueue_feature(host, HostToken, 101 + InteropPingTimeoutMs, frame, frame_length, &event) == InteropPeerSuccess);
    expect_event(&event, InteropPeerEventDraining, InteropPeerDraining);
    Check(interop_peer_enqueue_feature(host, HostToken, 101 + InteropPingTimeoutMs + 1, frame, frame_length, &event) == InteropPeerWrongPhase);

    Check(interop_peer_destroy(&guest) == InteropPeerSuccess);
    Check(interop_peer_destroy(&host) == InteropPeerSuccess);
}

int main(void)
{
    test_invalid_create_and_runtime_pointer_guards();
    test_null_owner_is_rejected_by_every_operation();
    test_zero_token_is_rejected_by_every_peer_operation();
    test_malformed_header_is_reported_as_terminal_feed_result();
    test_operation_outputs_tokens_and_callback_failures();
    test_timer_and_empty_queue_error_paths();
    test_roundtrip_and_feature_copy();
    test_local_errors_drain_abort_and_closed_state();
    test_host_mapping_rejection_drains_without_readiness();
    test_drain_aborts_unfinished_ack();
    test_outbound_feature_admission();
    puts("control peer C ABI tests passed");
    return 0;
}
