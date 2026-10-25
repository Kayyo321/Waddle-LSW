/**
 * @file session_local_handler_test.c
 * @brief Unit, socket, and fault tests for the session.sock command handler.
 *
 * This file includes the handler source so private rules are testable directly. The
 * feature sender entry points are supplied here as scripted fakes; the production runtime
 * provides the real ones in session_control_host.c.
 */
#include "../../src/interop/session_local_handler.c"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#define Check(condition) assert((condition) && #condition)

static void test_init_rejects_bad_input(void)
{
    session_local_handler_t handler;
    session_local_config_t config = {
        .runtime_dir = "/run/user/1000",
        .device_name = "alpha",
        .lock_held_proof = 1,
        .reserved_zero = 0,
    };
    memset(&handler, 0xAB, sizeof(handler));
    Check(session_local_handler_init(NULL, &config) == SessionLocalInvalidArgument);
    Check(session_local_handler_init(&handler, NULL) == SessionLocalInvalidArgument);

    config.reserved_zero = 1;
    Check(session_local_handler_init(&handler, &config) == SessionLocalRefused);
    config.reserved_zero = 0;

    config.runtime_dir = "run/user/1000";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.runtime_dir = NULL;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.runtime_dir = "/run/user/1000";

    config.device_name = NULL;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a/b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a.b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    char longest[SessionLocalDeviceNameMax + 2];
    memset(longest, 'x', sizeof(longest) - 1);
    longest[sizeof(longest) - 1] = '\0';
    config.device_name = longest;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    longest[SessionLocalDeviceNameMax] = '\0';
    Check(session_local_handler_init(&handler, &config) == SessionLocalSuccess);
    config.device_name = "alpha";

    /* A runtime directory this long pushes the socket path past sun_path. */
    char deep[256];
    memset(deep, 'd', sizeof(deep) - 1);
    deep[0] = '/';
    deep[sizeof(deep) - 1] = '\0';
    config.runtime_dir = deep;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);

    unsigned char before[sizeof(handler)];
    memset(&handler, 0x5C, sizeof(handler));
    memcpy(before, &handler, sizeof(handler));
    config.runtime_dir = "/run/user/1000";
    config.device_name = "a-name-of-sixty-four-characters-is-one-byte-over-the-limit-for-devices-x";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    Check(memcmp(before, &handler, sizeof(handler)) == 0);
}

static void test_init_fills_paths_and_defaults(void)
{
    session_local_handler_t *handler = calloc(1, sizeof(*handler));
    Check(handler != NULL);
    session_local_config_t config = {
        .runtime_dir = "/run/user/1000",
        .device_name = "alpha-1_x",
        .lock_held_proof = 7,
        .reserved_zero = 0,
    };
    Check(session_local_handler_init(handler, &config) == SessionLocalSuccess);
    Check(strcmp(handler->directory_path, "/run/user/1000/waddle/alpha-1_x") == 0);
    Check(strcmp(handler->socket_path, "/run/user/1000/waddle/alpha-1_x/session.sock") == 0);
    Check(handler->lock_held_proof == 7);
    Check(handler->expected_uid == (uint32_t)geteuid());
    Check(handler->listen_fd == -1 && handler->epoll_fd == -1);
    Check(handler->socket_created == 0 && handler->bound == 0 && handler->stopping == 0 && handler->closed == 0);
    Check(handler->next_serial == 1);
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        Check(handler->clients[index].fd == -1);
        Check(handler->clients[index].serial == 0);
    }
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) Check(handler->pending[index].in_use == 0);
    session_local_stats_t stats;
    session_local_handler_stats(handler, &stats);
    Check(stats.accepted == 0 && stats.launches_forwarded == 0 && stats.accept_errors == 0);
    session_local_handler_stats(NULL, &stats);
    session_local_handler_stats(handler, NULL);
    session_local_handler_destroy(NULL);
    free(handler);
}

static void test_directory_decision(void)
{
    Check(session_local_directory_decision(1, 0, 1000, 0700, 1000) == SessionLocalDirectoryUsable);
    Check(session_local_directory_decision(1, 0, 1000, 040700, 1000) == SessionLocalDirectoryUsable);
    Check(session_local_directory_decision(0, 0, 1000, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 1, 1000, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 0, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0755, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0500, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0710, 1000) == SessionLocalDirectoryRefused);
}

static void test_stale_and_peer_decisions(void)
{
    Check(session_local_stale_decision(ECONNREFUSED, 1) == SessionLocalStaleUnlink);
    Check(session_local_stale_decision(ECONNREFUSED, 0) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(0, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EAGAIN, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EINPROGRESS, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EACCES, 1) == SessionLocalStaleRefuse);

    Check(session_local_peer_decision(1000, 1000) == SessionLocalPeerAllowed);
    Check(session_local_peer_decision(0, 1000) == SessionLocalPeerRefused);
    Check(session_local_peer_decision(1001, 1000) == SessionLocalPeerRefused);
}

static void test_send_outcome_mapping(void)
{
    Check(session_local_send_outcome(HostFeatureSuccess) == SessionLocalOutcomeAccepted);
    Check(session_local_send_outcome(HostFeatureQueueFull) == SessionLocalOutcomeBusy);
    Check(session_local_send_outcome(HostFeatureRequestIdsFull) == SessionLocalOutcomeBusy);
    Check(session_local_send_outcome(HostFeatureNotNegotiated) == SessionLocalOutcomeUnsupported);
    Check(session_local_send_outcome(HostFeatureDraining) == SessionLocalOutcomeCancelled);
    Check(session_local_send_outcome(HostFeatureNotReady) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureInvalidFrame) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureUnsupportedFrame) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureOutOfMemory) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureClosed) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureTransportFailed) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(99) == SessionLocalOutcomeError);
}

/* ---- Scripted feature sender (replaces the runtime entry points) ---- */

static int fake_send_result = HostFeatureSuccess;
static int fake_release_result = HostFeatureSuccess;
static uint32_t fake_next_request_id = 1;
static uint8_t fake_last_frame[InteropMaxFrameBytes];
static size_t fake_last_length;
static uint32_t fake_sends;
static uint32_t fake_released[64];
static size_t fake_released_count;

int interop_host_feature_send_request(interop_host_feature_sender_t *sender, const uint8_t *frame,
    size_t length, uint32_t *out_request_id)
{
    (void)sender;
    if (fake_send_result != HostFeatureSuccess) return fake_send_result;
    assert(length <= sizeof(fake_last_frame));
    memcpy(fake_last_frame, frame, length);
    fake_last_length = length;
    ++fake_sends;
    *out_request_id = fake_next_request_id++;
    return HostFeatureSuccess;
}

int interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length)
{
    (void)sender;
    (void)frame;
    (void)length;
    return HostFeatureSuccess;
}

int interop_host_feature_release_request(interop_host_feature_sender_t *sender, uint32_t request_id)
{
    (void)sender;
    if (fake_released_count < sizeof(fake_released) / sizeof(fake_released[0]))
        fake_released[fake_released_count++] = request_id;
    return fake_release_result;
}

static interop_host_feature_sender_t *const FakeSender = (interop_host_feature_sender_t *)(uintptr_t)0x1;

static void fake_reset(void)
{
    fake_send_result = HostFeatureSuccess;
    fake_release_result = HostFeatureSuccess;
    fake_next_request_id = 1;
    fake_sends = 0;
    fake_last_length = 0;
    fake_released_count = 0;
}

/* ---- Frame builders and readers ---- */

static size_t build_local_launch(uint8_t *frame, size_t capacity, uint32_t request_id, uint64_t kind,
    const char *key_or_path, const char *arguments, const char *working_dir, const char *token)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    if (kind == InteropLaunchApp) assert(interop_put_bytes(&writer, InteropTagAppKey, key_or_path, (uint16_t)strlen(key_or_path)) == 0);
    else assert(interop_put_bytes(&writer, InteropTagTargetPath, key_or_path, (uint16_t)strlen(key_or_path)) == 0);
    if (arguments != NULL) assert(interop_put_bytes(&writer, InteropTagArguments, arguments, (uint16_t)strlen(arguments)) == 0);
    if (working_dir != NULL) assert(interop_put_bytes(&writer, InteropTagWorkingDir, working_dir, (uint16_t)strlen(working_dir)) == 0);
    assert(interop_put_uint(&writer, InteropTagLaunchKind, kind) == 0);
    if (token != NULL) assert(interop_put_bytes(&writer, InteropTagActivationToken, token, (uint16_t)strlen(token)) == 0);
    assert(interop_writer_finish(&writer, InteropMsgLocalLaunch, request_id, InteropFromLocal, &length) == 0);
    return length;
}

static size_t build_guest_result(uint8_t *frame, size_t capacity, uint32_t request_id, uint32_t status,
    uint32_t process_id, const char *message, const char *guest_token)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagProcessId, process_id) == 0);
    if (guest_token != NULL) assert(interop_put_bytes(&writer, InteropTagActivationToken, guest_token, (uint16_t)strlen(guest_token)) == 0);
    assert(interop_put_uint(&writer, InteropTagStatus, status) == 0);
    if (message != NULL) assert(interop_put_bytes(&writer, InteropTagMessage, message, (uint16_t)strlen(message)) == 0);
    assert(interop_writer_finish(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest, &length) == 0);
    return length;
}

/** @brief Pop the first reply frame from a client transmit buffer; returns its header and payload. */
static void pop_reply(session_local_handler_t *handler, uint32_t index, interop_header_t *header,
    uint8_t *payload, size_t *payload_length)
{
    session_local_client_t *client = &handler->clients[index];
    assert(client->tx_length - client->tx_offset >= InteropHeaderBytes);
    assert(interop_header_decode(client->tx + client->tx_offset, client->tx_length - client->tx_offset, header) == 0);
    size_t frame_length = InteropHeaderBytes + header->length;
    assert(client->tx_length - client->tx_offset >= frame_length);
    *payload_length = header->length;
    memcpy(payload, client->tx + client->tx_offset + InteropHeaderBytes, header->length);
    memmove(client->tx, client->tx + client->tx_offset + frame_length, client->tx_length - client->tx_offset - frame_length);
    client->tx_length -= client->tx_offset + frame_length;
    client->tx_offset = 0;
}

static uint64_t pop_status(session_local_handler_t *handler, uint32_t index, uint32_t expected_type,
    uint32_t expected_request_id)
{
    interop_header_t header;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    pop_reply(handler, index, &header, payload, &payload_length);
    assert(header.type == expected_type);
    assert(header.request_id == expected_request_id);
    uint64_t status = 0;
    assert(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0);
    return status;
}

/** @brief Attach a fake client to a slot (descriptor is /dev/null; no socket I/O in these tests). */
static uint64_t attach_client(session_local_handler_t *handler, uint32_t index)
{
    session_local_client_t *client = &handler->clients[index];
    assert(client->fd < 0);
    client->fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(client->fd >= 0);
    client->serial = handler->next_serial++;
    client->rx_length = 0;
    client->tx_length = 0;
    client->tx_offset = 0;
    return client->serial;
}

static void detach_client(session_local_handler_t *handler, uint32_t index)
{
    client_close(handler, index);
}

static session_local_handler_t *new_handler(void)
{
    session_local_handler_t *handler = calloc(1, sizeof(*handler));
    session_local_config_t config = {
        .runtime_dir = "/run/user/1000",
        .device_name = "alpha",
        .lock_held_proof = 1,
        .reserved_zero = 0,
    };
    assert(handler != NULL);
    assert(session_local_handler_init(handler, &config) == SessionLocalSuccess);
    return handler;
}

static void free_handler(session_local_handler_t *handler)
{
    session_local_handler_destroy(handler);
    free(handler);
}

static size_t launch_payload_length(size_t length)
{
    return length - InteropHeaderBytes;
}

/* ---- Launch forwarding and correlation ---- */

static void test_forwarding_builds_launch_request(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 9, InteropLaunchPath, "Z:\\notes.exe",
        "--flag 'a b'", "Z:\\work", "tok-1");
    forward_launch(handler, FakeSender, 0, serial, 9, frame + InteropHeaderBytes,
        launch_payload_length(length), 1000);
    Check(fake_sends == 1);
    Check(handler->stats.launches_forwarded == 1);
    interop_header_t header;
    Check(interop_header_decode(fake_last_frame, fake_last_length, &header) == 0);
    Check(header.type == InteropMsgLaunchRequest && header.request_id == 0);
    Check(interop_payload_validate(header.type, InteropFromHost, fake_last_frame + InteropHeaderBytes,
        fake_last_length - InteropHeaderBytes) == 0);
    const uint8_t *payload = fake_last_frame + InteropHeaderBytes;
    size_t payload_length = fake_last_length - InteropHeaderBytes;
    const uint8_t *value = NULL;
    uint16_t value_length = 0;
    Check(interop_field_get(payload, payload_length, InteropTagTargetPath, 0, &value, &value_length) == 0);
    Check(value_length == strlen("Z:\\notes.exe") && memcmp(value, "Z:\\notes.exe", value_length) == 0);
    Check(interop_field_get(payload, payload_length, InteropTagArguments, 0, &value, &value_length) == 0);
    Check(value_length == strlen("--flag 'a b'"));
    Check(interop_field_get(payload, payload_length, InteropTagWorkingDir, 0, &value, &value_length) == 0);
    Check(value_length == strlen("Z:\\work"));
    Check(interop_field_get(payload, payload_length, InteropTagAppKey, 0, &value, &value_length) == 1);
    uint64_t kind = 0;
    Check(interop_field_uint(payload, payload_length, InteropTagLaunchKind, 1, &kind) == 0 && kind == InteropLaunchPath);
    Check(interop_field_get(payload, payload_length, InteropTagActivationToken, 0, &value, &value_length) == 0);
    Check(value_length == 5 && memcmp(value, "tok-1", 5) == 0);
    Check(handler->pending[0].in_use == 1 && handler->pending[0].guest_request_id == 1);
    Check(handler->pending[0].local_request_id == 9);
    Check(handler->pending[0].deadline_ms == 1000 + SessionLocalLaunchTimeoutMs);
    Check(handler->pending[0].token_length == 5 && strcmp((const char *)handler->pending[0].token, "tok-1") == 0);
    Check(handler->clients[0].tx_length == 0);

    /* App launches carry the key and no target path; absent optional fields are not invented. */
    length = build_local_launch(frame, sizeof(frame), 10, InteropLaunchApp, "a-Microsoft_Explorer", NULL, NULL, NULL);
    forward_launch(handler, FakeSender, 0, serial, 10, frame + InteropHeaderBytes,
        launch_payload_length(length), 2000);
    Check(fake_sends == 2);
    payload = fake_last_frame + InteropHeaderBytes;
    payload_length = fake_last_length - InteropHeaderBytes;
    Check(interop_field_get(payload, payload_length, InteropTagAppKey, 0, &value, &value_length) == 0);
    Check(value_length == strlen("a-Microsoft_Explorer"));
    Check(interop_field_get(payload, payload_length, InteropTagTargetPath, 0, &value, &value_length) == 1);
    Check(interop_field_get(payload, payload_length, InteropTagActivationToken, 0, &value, &value_length) == 1);
    Check(handler->pending[1].token_length == 0 && handler->pending[1].token[0] == '\0');
    free_handler(handler);
}

static void test_refused_sends_map_to_replies(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 3, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, NULL);
    const int results[] = {HostFeatureQueueFull, HostFeatureRequestIdsFull, HostFeatureNotNegotiated, HostFeatureDraining, HostFeatureNotReady};
    const uint64_t statuses[] = {InteropBusy, InteropBusy, InteropUnsupported, InteropCancelled};
    for (size_t index = 0; index < 4; ++index) {
        fake_send_result = results[index];
        forward_launch(handler, FakeSender, 0, serial, 3, frame + InteropHeaderBytes,
            launch_payload_length(length), 500);
        Check(pop_status(handler, 0, InteropMsgLaunchResult, 3) == statuses[index]);
    }
    fake_send_result = results[4];
    forward_launch(handler, FakeSender, 0, serial, 3, frame + InteropHeaderBytes,
        launch_payload_length(length), 500);
    interop_header_t header;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    pop_reply(handler, 0, &header, payload, &payload_length);
    Check(header.type == InteropMsgError && header.request_id == 3);
    const uint8_t *message = NULL;
    uint16_t message_length = 0;
    Check(interop_field_get(payload, payload_length, InteropTagMessage, 0, &message, &message_length) == 0);
    Check(message_length == strlen("launch not forwarded") && memcmp(message, "launch not forwarded", message_length) == 0);
    uint64_t code = 0;
    Check(interop_field_uint(payload, payload_length, InteropTagErrorCode, 4, &code) == 0 && code == InteropErrorUnexpected);

    Check(handler->stats.launches_busy == 2 && handler->stats.launches_unsupported == 1);
    Check(handler->stats.launches_cancelled == 1 && handler->stats.launch_errors == 1);
    Check(handler->stats.launches_forwarded == 0);
    Check(fake_sends == 0);
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) Check(handler->pending[index].in_use == 0);
    free_handler(handler);
}

static void test_outstanding_and_pending_limits(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 1, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, NULL);
    for (uint32_t index = 0; index < SessionLocalMaxOutstanding; ++index) {
        forward_launch(handler, FakeSender, 0, serial, index + 1, frame + InteropHeaderBytes,
            launch_payload_length(length), 0);
    }
    Check(fake_sends == SessionLocalMaxOutstanding);
    forward_launch(handler, FakeSender, 0, serial, 77, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    Check(fake_sends == SessionLocalMaxOutstanding);
    Check(pop_status(handler, 0, InteropMsgLaunchResult, 77) == InteropBusy);

    /* A full correlation table refuses before any send. */
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) {
        if (!handler->pending[index].in_use) {
            handler->pending[index].in_use = 1;
            handler->pending[index].client_serial = 0xFFFF;
        }
    }
    uint64_t other = attach_client(handler, 1);
    forward_launch(handler, FakeSender, 1, other, 78, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    Check(fake_sends == SessionLocalMaxOutstanding);
    Check(pop_status(handler, 1, InteropMsgLaunchResult, 78) == InteropBusy);
    free_handler(handler);
}

static void test_stop_reply_echoes_request(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    Check(reply_stop_ok(handler, 0, serial, 7) == 1);
    interop_header_t header;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    pop_reply(handler, 0, &header, payload, &payload_length);
    Check(header.type == InteropMsgLocalStopResult && header.request_id == 7);
    Check(interop_payload_validate(header.type, InteropFromLocal, payload, payload_length) == 0);
    uint64_t status = 99;
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropOk);
    detach_client(handler, 0);
    Check(reply_stop_ok(handler, 0, serial, 8) == 0);
    Check(handler->stats.replies_dropped == 1);
    free_handler(handler);
}

static void test_stopping_cancels_new_launches(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    handler->stopping = 1;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 5, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, NULL);
    forward_launch(handler, FakeSender, 0, serial, 5, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    Check(fake_sends == 0);
    Check(pop_status(handler, 0, InteropMsgLaunchResult, 5) == InteropCancelled);
    free_handler(handler);
}

static void test_guest_result_delivers_and_stores_token(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 4, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, "client-token");
    forward_launch(handler, FakeSender, 0, serial, 4, frame + InteropHeaderBytes,
        launch_payload_length(length), 100);
    Check(handler->pending[0].in_use == 1);
    uint32_t guest_id = handler->pending[0].guest_request_id;

    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), guest_id, InteropOk, 4242,
        "started", "guest-token");
    interop_header_t header;
    Check(interop_header_decode(result, result_length, &header) == 0);
    Check(handle_launch_result(handler, FakeSender, header.request_id, result, result_length) == 0);
    Check(fake_released_count == 1 && fake_released[0] == guest_id);
    Check(handler->stats.results_delivered == 1 && handler->stats.token_stored == 1);
    Check(handler->pending[0].in_use == 0);

    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    pop_reply(handler, 0, &reply, payload, &payload_length);
    Check(reply.type == InteropMsgLaunchResult && reply.request_id == 4);
    uint64_t process_id = 0;
    uint64_t status = 99;
    Check(interop_field_uint(payload, payload_length, InteropTagProcessId, 4, &process_id) == 0 && process_id == 4242);
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropOk);
    const uint8_t *message = NULL;
    uint16_t message_length = 0;
    Check(interop_field_get(payload, payload_length, InteropTagMessage, 0, &message, &message_length) == 0);
    Check(message_length == 7 && memcmp(message, "started", 7) == 0);
    Check(interop_field_get(payload, payload_length, InteropTagActivationToken, 0, &message, &message_length) == 1);

    /* The stored token is the client's, keyed by the guest process id; the guest's token is ignored. */
    uint8_t taken[InteropActivationTokenBufferBytes];
    size_t taken_length = 0;
    Check(interop_activation_take(&handler->tokens, 4242, 200, taken, &taken_length) == InteropActivationTakeFound);
    Check(taken_length == 12 && memcmp(taken, "client-token", 12) == 0 && taken[12] == '\0');
    free_handler(handler);
}

static void test_guest_failure_status_stores_nothing(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 6, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, "tok-f");
    forward_launch(handler, FakeSender, 0, serial, 6, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    uint32_t guest_id = handler->pending[0].guest_request_id;
    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), guest_id, InteropGuestError, 0, "no", NULL);
    Check(handle_launch_result(handler, FakeSender, guest_id, result, result_length) == 0);
    Check(handler->stats.token_stored == 0 && handler->stats.token_refused == 0);
    uint8_t taken[InteropActivationTokenBufferBytes];
    size_t taken_length = 0;
    Check(interop_activation_take(&handler->tokens, 0, 0, taken, &taken_length) != InteropActivationTakeFound);
    Check(pop_status(handler, 0, InteropMsgLaunchResult, 6) == InteropGuestError);
    free_handler(handler);
}

static void test_unknown_duplicate_and_malformed_results(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 2, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, NULL);
    forward_launch(handler, FakeSender, 0, serial, 2, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    uint32_t guest_id = handler->pending[0].guest_request_id;
    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), 999, InteropOk, 1, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, 999, result, result_length) == 0);
    Check(handler->stats.ignored_results == 1 && fake_released_count == 0);

    result_length = build_guest_result(result, sizeof(result), guest_id, InteropOk, 1, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, guest_id, result, result_length) == 0);
    Check(handle_launch_result(handler, FakeSender, guest_id, result, result_length) == 0);
    Check(handler->stats.ignored_results == 2 && fake_released_count == 1);
    Check(handler->stats.results_delivered == 1);

    /* A LaunchResult without status is malformed; the run must fail. */
    forward_launch(handler, FakeSender, 0, serial, 8, frame + InteropHeaderBytes,
        launch_payload_length(length), 0);
    uint32_t second = handler->pending[0].guest_request_id;
    uint8_t bare[] = {
        0x57, 0x49, 0x4F, 0x50, 0x02, 0x00, 0x01, 0x07, 0x08, 0x00, 0x00, 0x00,
        (uint8_t)second, 0x00, 0x00, 0x00,
        0x09, 0x00, 0x04, 0x00, 0x2A, 0x00, 0x00, 0x00,
    };
    Check(handle_launch_result(handler, FakeSender, second, bare, sizeof(bare)) == 1);
    Check(handler->pending[0].in_use == 1);
    free_handler(handler);
}

static void test_token_refusals_and_disconnect(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 1, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, "same");
    forward_launch(handler, FakeSender, 0, serial, 1, frame + InteropHeaderBytes, launch_payload_length(length), 0);
    forward_launch(handler, FakeSender, 0, serial, 2, frame + InteropHeaderBytes, launch_payload_length(length), 0);
    uint32_t first = handler->pending[0].guest_request_id;
    uint32_t second = handler->pending[1].guest_request_id;
    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), first, InteropOk, 0, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, first, result, result_length) == 0);
    Check(handler->stats.token_refused == 1 && handler->stats.token_stored == 0);

    result_length = build_guest_result(result, sizeof(result), second, InteropOk, 555, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, second, result, result_length) == 0);
    Check(handler->stats.token_stored == 1);

    /* A disconnected client: the result releases the identifier and the reply is dropped. */
    forward_launch(handler, FakeSender, 0, serial, 3, frame + InteropHeaderBytes, launch_payload_length(length), 0);
    uint32_t third = handler->pending[0].guest_request_id;
    Check(handler->pending[0].in_use == 1);
    detach_client(handler, 0);
    result_length = build_guest_result(result, sizeof(result), third, InteropGuestError, 0, "late", NULL);
    Check(handle_launch_result(handler, FakeSender, third, result, result_length) == 0);
    Check(fake_released_count == 3 && handler->stats.replies_dropped == 1);
    free_handler(handler);
}

static void test_duplicate_live_token_is_refused(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    uint64_t serial = attach_client(handler, 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 1, InteropLaunchPath, "Z:\\a.exe", NULL, NULL, "tk");
    forward_launch(handler, FakeSender, 0, serial, 1, frame + InteropHeaderBytes, launch_payload_length(length), 0);
    forward_launch(handler, FakeSender, 0, serial, 2, frame + InteropHeaderBytes, launch_payload_length(length), 0);
    uint32_t first = handler->pending[0].guest_request_id;
    uint32_t second = handler->pending[1].guest_request_id;
    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), first, InteropOk, 9, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, first, result, result_length) == 0);
    result_length = build_guest_result(result, sizeof(result), second, InteropOk, 10, NULL, NULL);
    Check(handle_launch_result(handler, FakeSender, second, result, result_length) == 0);
    Check(handler->stats.token_stored == 1 && handler->stats.token_refused == 1);
    free_handler(handler);
}

/* ---- Real sockets: bind, accept, frames ---- */

static char test_runtime[128];

static void make_runtime(void)
{
    strcpy(test_runtime, "/tmp/session-local-XXXXXX");
    assert(mkdtemp(test_runtime) != NULL);
    assert(chmod(test_runtime, 0700) == 0);
}

static void remove_tree(const char *path)
{
    char command[256];
    assert(snprintf(command, sizeof(command), "rm -rf '%s'", path) > 0);
    assert(system(command) == 0);
}

static session_local_handler_t *open_handler(const char *device, uint32_t proof)
{
    session_local_handler_t *handler = calloc(1, sizeof(*handler));
    session_local_config_t config = {
        .runtime_dir = test_runtime,
        .device_name = device,
        .lock_held_proof = proof,
        .reserved_zero = 0,
    };
    assert(handler != NULL);
    assert(session_local_handler_init(handler, &config) == SessionLocalSuccess);
    return handler;
}

static int socket_path_mode(const char *path, mode_t *mode)
{
    struct stat status;
    if (lstat(path, &status) != 0) return -1;
    *mode = status.st_mode;
    return 0;
}

static int connect_path(const char *path)
{
    struct sockaddr_un address;
    struct timeval timeout = {.tv_sec = 2};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static void send_all(int fd, const void *bytes, size_t length)
{
    const uint8_t *data = bytes;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
        assert(sent > 0);
        offset += (size_t)sent;
    }
}

/** @brief Read one whole reply frame from a client socket; returns false on end of stream. */
static bool recv_frame(int fd, interop_header_t *header, uint8_t *payload, size_t *payload_length)
{
    uint8_t head[InteropHeaderBytes];
    size_t got = 0;
    while (got < sizeof(head)) {
        ssize_t received = recv(fd, head + got, sizeof(head) - got, 0);
        if (received <= 0) return false;
        got += (size_t)received;
    }
    assert(interop_header_decode(head, sizeof(head), header) == 0);
    got = 0;
    while (got < header->length) {
        ssize_t received = recv(fd, payload + got, header->length - got, 0);
        if (received <= 0) return false;
        got += (size_t)received;
    }
    *payload_length = header->length;
    return true;
}

/** @brief One runtime-style poll cycle through the callback table. */
static void pump_once(const session_handler_t *table, uint64_t now_ms)
{
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 0;
    assert(table->poll_fds(table->context, fds, SessionHandlerMaxDescriptors, &count) == 0);
    assert(count <= SessionHandlerMaxDescriptors);
    if (count == 0) return;
    /* Two rounds: a connection accepted in one round has its first bytes reported in the next. */
    for (int round = 0; round < 2; ++round) {
        fds[0].revents = 0;
        (void)poll(fds, count, round == 0 ? 20 : 0);
        if (fds[0].revents != 0) assert(table->on_fds(table->context, fds, count, now_ms, FakeSender) == 0);
    }
}

static size_t count_open_descriptors(void)
{
    size_t count = 0;
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    while (readdir(directory) != NULL) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void test_ready_creates_private_socket_and_directories(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(strcmp(table.name, "session_local") == 0 && table.capabilities == InteropCapLaunch);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    Check(handler->bound == 1 && handler->socket_created == 1);
    mode_t mode = 0;
    char path[256];
    Check(snprintf(path, sizeof(path), "%s/waddle", test_runtime) > 0);
    Check(socket_path_mode(path, &mode) == 0 && (mode & 07777) == 0700);
    Check(snprintf(path, sizeof(path), "%s/waddle/alpha", test_runtime) > 0);
    Check(socket_path_mode(path, &mode) == 0 && (mode & 07777) == 0700);
    Check(snprintf(path, sizeof(path), "%s/waddle/alpha/session.sock", test_runtime) > 0);
    Check(socket_path_mode(path, &mode) == 0 && S_ISSOCK(mode) && (mode & 07777) == 0600);
    Check(table.on_ready(table.context, 0, FakeSender) == 1);

    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 0;
    Check(table.poll_fds(table.context, fds, SessionHandlerMaxDescriptors, &count) == 0 && count == 1);
    Check(fds[0].fd == handler->epoll_fd && fds[0].events == POLLIN && fds[0].revents == 0);
    session_local_handler_destroy(handler);
    Check(socket_path_mode(path, &mode) != 0);
    free(handler);
    remove_tree(test_runtime);
}

static void test_ready_refuses_unsafe_directories_and_paths(void)
{
    fake_reset();
    make_runtime();
    char waddle[256];
    char device[256];
    char socket_path[256];
    Check(snprintf(waddle, sizeof(waddle), "%s/waddle", test_runtime) > 0);
    Check(snprintf(device, sizeof(device), "%s/waddle/alpha", test_runtime) > 0);
    Check(snprintf(socket_path, sizeof(socket_path), "%s/session.sock", device) > 0);

    /* A group-readable device directory is refused and left as it is. */
    Check(mkdir(waddle, 0700) == 0 && mkdir(device, 0755) == 0);
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 1);
    mode_t mode = 0;
    Check(socket_path_mode(device, &mode) == 0 && (mode & 07777) == 0755);
    Check(!handler->bound && !handler->socket_created);
    session_local_handler_destroy(handler);
    free(handler);
    Check(rmdir(device) == 0);

    /* A symbolic link where the device directory belongs is refused and never followed. */
    Check(symlink("/tmp", device) == 0);
    handler = open_handler("alpha", 1);
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 1);
    Check(socket_path_mode(device, &mode) == 0 && S_ISLNK(mode));
    session_local_handler_destroy(handler);
    free(handler);
    Check(unlink(device) == 0);

    /* A regular file at the socket path is never unlinked. */
    Check(mkdir(device, 0700) == 0);
    int fd = open(socket_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    Check(fd >= 0);
    Check(write(fd, "keep", 4) == 4);
    Check(close(fd) == 0);
    handler = open_handler("alpha", 1);
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 1);
    Check(socket_path_mode(socket_path, &mode) == 0 && S_ISREG(mode));
    session_local_handler_destroy(handler);
    free(handler);
    Check(unlink(socket_path) == 0);
    remove_tree(test_runtime);
}

static void test_stale_socket_policy(void)
{
    fake_reset();
    make_runtime();
    char socket_path[256];
    char device[256];
    Check(snprintf(device, sizeof(device), "%s/waddle/alpha", test_runtime) > 0);
    Check(snprintf(socket_path, sizeof(socket_path), "%s/session.sock", device) > 0);
    session_handler_t table;

    /* Leave a socket file with no listener: the crash leftover. */
    Check(snprintf(device, sizeof(device), "%s/waddle", test_runtime) > 0);
    Check(mkdir(device, 0700) == 0);
    Check(snprintf(device, sizeof(device), "%s/waddle/alpha", test_runtime) > 0);
    Check(mkdir(device, 0700) == 0);
    int stale = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1u);
    Check(stale >= 0 && bind(stale, (const struct sockaddr *)&address, sizeof(address)) == 0);
    Check(close(stale) == 0);

    /* Without the singleton lock proof the stale file is never unlinked. */
    session_local_handler_t *handler = open_handler("alpha", 0);
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 1);
    mode_t mode = 0;
    Check(socket_path_mode(socket_path, &mode) == 0 && S_ISSOCK(mode));
    session_local_handler_destroy(handler);
    free(handler);

    /* With proof, the stale file is replaced, and this handler owns and removes it. */
    handler = open_handler("alpha", 1);
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    Check(handler->socket_created == 1);
    session_local_handler_destroy(handler);
    free(handler);
    Check(socket_path_mode(socket_path, &mode) != 0);

    /* A live listener is never unlinked, even with proof. */
    handler = open_handler("alpha", 1);
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    session_handler_t second_table;
    session_local_handler_t *rival = open_handler("alpha", 1);
    session_local_handler_table(rival, &second_table);
    Check(second_table.on_ready(second_table.context, 0, FakeSender) == 1);
    Check(rival->socket_created == 0);
    session_local_handler_destroy(rival);
    free(rival);
    Check(socket_path_mode(socket_path, &mode) == 0 && S_ISSOCK(mode));
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}


static size_t build_frame_with(uint8_t *frame, size_t capacity, uint16_t type, uint32_t request_id, uint32_t direction)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_writer_finish(&writer, type, request_id, (interop_direction_t)direction, &length) == 0);
    return length;
}

static void test_client_launch_roundtrip_and_token(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int client = connect_path(handler->socket_path);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 11, InteropLaunchPath, "Z:\\a.exe", "x", NULL, "tok-c");
    send_all(client, frame, length);
    pump_once(&table, 1000);
    Check(handler->stats.accepted == 1 && fake_sends == 1);
    Check(handler->clients[0].frame_seen == 1);

    uint8_t result[InteropMaxFrameBytes];
    size_t result_length = build_guest_result(result, sizeof(result), 1, InteropOk, 77, "ok", NULL);
    Check(table.on_frame(table.context, InteropMsgLaunchResult, 1, result, result_length, FakeSender) == 0);
    Check(table.on_tick(table.context, 1100, FakeSender) == 0);
    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    Check(recv_frame(client, &reply, payload, &payload_length));
    Check(reply.type == InteropMsgLaunchResult && reply.request_id == 11);
    uint64_t status = 9;
    uint64_t process_id = 0;
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropOk);
    Check(interop_field_uint(payload, payload_length, InteropTagProcessId, 4, &process_id) == 0 && process_id == 77);
    uint8_t taken[InteropActivationTokenBufferBytes];
    size_t taken_length = 0;
    Check(interop_activation_take(&handler->tokens, 77, 1100, taken, &taken_length) == InteropActivationTakeFound);
    Check(taken_length == 5 && memcmp(taken, "tok-c", 5) == 0);

    /* Split and pipelined frames are reassembled and dispatched in order. */
    length = build_local_launch(frame, sizeof(frame), 12, InteropLaunchPath, "Z:\\b.exe", NULL, NULL, NULL);
    send_all(client, frame, 10);
    pump_once(&table, 1200);
    Check(fake_sends == 1);
    send_all(client, frame + 10, length - 10);
    pump_once(&table, 1210);
    Check(fake_sends == 2);
    uint8_t twice[2 * InteropMaxFrameBytes / 4];
    size_t one = build_local_launch(frame, sizeof(frame), 13, InteropLaunchPath, "Z:\\c.exe", NULL, NULL, NULL);
    size_t two = build_local_launch(twice + one, sizeof(twice) - one, 14, InteropLaunchPath, "Z:\\d.exe", NULL, NULL, NULL);
    memcpy(twice, frame, one);
    send_all(client, twice, one + two);
    pump_once(&table, 1220);
    Check(fake_sends == 4 && handler->stats.launches_forwarded == 4);

    Check(close(client) == 0);
    pump_once(&table, 1300);
    Check(handler->clients[0].fd == -1 && handler->stats.closed_io == 1);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_client_protocol_violations_close(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;

    /* Bad magic. */
    int client = connect_path(handler->socket_path);
    memset(frame, 'x', InteropHeaderBytes);
    send_all(client, frame, InteropHeaderBytes);
    pump_once(&table, 10);
    Check(handler->stats.closed_malformed == 1);
    uint8_t byte;
    Check(recv(client, &byte, 1, 0) == 0);
    Check(close(client) == 0);

    /* Wrong type: a guest-only frame sent on the local socket. */
    client = connect_path(handler->socket_path);
    length = build_frame_with(frame, sizeof(frame), InteropMsgPong, 1, InteropFromGuest);
    send_all(client, frame, length);
    pump_once(&table, 20);
    Check(handler->stats.closed_unknown_type == 1);
    Check(recv(client, &byte, 1, 0) == 0);
    Check(close(client) == 0);

    /* Zero request identifier. */
    client = connect_path(handler->socket_path);
    length = build_local_launch(frame, sizeof(frame), 0, InteropLaunchPath, "Z:\\z.exe", NULL, NULL, NULL);
    send_all(client, frame, length);
    pump_once(&table, 30);
    Check(handler->stats.closed_zero_request == 1);
    Check(recv(client, &byte, 1, 0) == 0);
    Check(close(client) == 0);

    /* Oversize announcement: the header alone is refused. */
    client = connect_path(handler->socket_path);
    length = build_local_launch(frame, sizeof(frame), 5, InteropLaunchPath, "Z:\\z.exe", NULL, NULL, NULL);
    frame[8] = 0x00;
    frame[9] = 0x10;
    frame[10] = 0x00;
    frame[11] = 0x00;
    send_all(client, frame, InteropHeaderBytes);
    pump_once(&table, 40);
    Check(handler->stats.closed_oversize == 1);
    Check(recv(client, &byte, 1, 0) == 0);
    Check(close(client) == 0);

    /* Unknown critical tag in a LocalLaunch payload fails validation. */
    client = connect_path(handler->socket_path);
    uint8_t bad[] = {
        0x57, 0x49, 0x4F, 0x50, 0x02, 0x00, 0x00, 0x7F, 0x08, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x99, 0x80, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04,
    };
    send_all(client, bad, sizeof(bad));
    pump_once(&table, 50);
    Check(handler->stats.closed_malformed == 2);
    Check(close(client) == 0);

    /* A LocalStop is not yet implemented here: its client is closed as an unknown type. */
    Check(handler->clients[0].fd == -1);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_max_clients_and_flush_order(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int clients[SessionLocalMaxClients + 1];
    for (uint32_t index = 0; index <= SessionLocalMaxClients; ++index) {
        clients[index] = connect_path(handler->socket_path);
        pump_once(&table, 100 + index);
    }
    Check(handler->stats.accepted == SessionLocalMaxClients);
    Check(handler->stats.refused_capacity == 1);
    uint8_t byte;
    Check(recv(clients[SessionLocalMaxClients], &byte, 1, 0) == 0);
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) Check(close(clients[index]) == 0);
    Check(close(clients[SessionLocalMaxClients]) == 0);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

int main(void)
{
    test_init_rejects_bad_input();
    test_init_fills_paths_and_defaults();
    test_directory_decision();
    test_stale_and_peer_decisions();
    test_send_outcome_mapping();
    test_forwarding_builds_launch_request();
    test_refused_sends_map_to_replies();
    test_outstanding_and_pending_limits();
    test_stop_reply_echoes_request();
    test_stopping_cancels_new_launches();
    test_guest_result_delivers_and_stores_token();
    test_guest_failure_status_stores_nothing();
    test_unknown_duplicate_and_malformed_results();
    test_token_refusals_and_disconnect();
    test_duplicate_live_token_is_refused();
    size_t descriptors_before = count_open_descriptors();
    test_ready_creates_private_socket_and_directories();
    test_ready_refuses_unsafe_directories_and_paths();
    test_stale_socket_policy();
    test_client_launch_roundtrip_and_token();
    test_client_protocol_violations_close();
    test_max_clients_and_flush_order();
    Check(count_open_descriptors() == descriptors_before);
    printf("session_local_handler_test: passed\n");
    return 0;
}
