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

/* Reason value passed to on_closing by the scripted tests (any interop_host_control_result_t). */
#define HostControlCancelledReason 7u

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

/* ---- Fault injection and scripted events (linker --wrap; see scripts/interop_lane_launch.mk) ---- */

extern int __real_socket(int domain, int type, int protocol);
extern int __real_bind(int fd, const struct sockaddr *address, socklen_t length);
extern int __real_listen(int fd, int backlog);
extern int __real_mkdir(const char *path, mode_t mode);
extern int __real_lstat(const char *path, struct stat *status);
extern int __real_unlink(const char *path);
extern ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
extern ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);
extern int __real_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
extern int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
extern int __real_epoll_create1(int flags);
extern int __real_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern int __real_epoll_wait(int epfd, struct epoll_event *events, int maximum, int timeout);

typedef enum fault_point_t {
    FaultSocket, FaultBind, FaultListen, FaultMkdir, FaultLstat, FaultUnlink, FaultSend, FaultRecv,
    FaultAccept, FaultGetsockopt, FaultEpollCreate, FaultEpollCtl, FaultEpollWait, FaultPoints
} fault_point_t;

static int fault_calls[FaultPoints];
static int fault_fail_at[FaultPoints];
static int fault_error[FaultPoints];
static int scripted_count = -1;
static struct epoll_event scripted_events[8];
static int fake_peer_uid = -1;

static void fault_arm(fault_point_t point, int nth, int error)
{
    fault_calls[point] = 0;
    fault_fail_at[point] = nth;
    fault_error[point] = error;
}

static void fault_reset(void)
{
    for (int index = 0; index < FaultPoints; ++index) {
        fault_calls[index] = 0;
        fault_fail_at[index] = 0;
        fault_error[index] = 0;
    }
    scripted_count = -1;
    fake_peer_uid = -1;
}

/** @brief True exactly once, on the armed call of a fault point; sets errno for the caller. */
static bool fault_fires(fault_point_t point)
{
    ++fault_calls[point];
    if (fault_fail_at[point] == 0 || fault_calls[point] != fault_fail_at[point]) return false;
    fault_fail_at[point] = 0;
    errno = fault_error[point];
    return true;
}

int __wrap_socket(int domain, int type, int protocol)
{
    if (fault_fires(FaultSocket)) return -1;
    return __real_socket(domain, type, protocol);
}

int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    if (fault_fires(FaultBind)) return -1;
    return __real_bind(fd, address, length);
}

int __wrap_listen(int fd, int backlog)
{
    if (fault_fires(FaultListen)) return -1;
    return __real_listen(fd, backlog);
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    if (fault_fires(FaultMkdir)) return -1;
    return __real_mkdir(path, mode);
}

int __wrap_lstat(const char *path, struct stat *status)
{
    if (fault_fires(FaultLstat)) return -1;
    return __real_lstat(path, status);
}

int __wrap_unlink(const char *path)
{
    if (fault_fires(FaultUnlink)) return -1;
    return __real_unlink(path);
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags)
{
    if (fault_fires(FaultSend)) return -1;
    return __real_send(fd, buffer, length, flags);
}

ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags)
{
    if (fault_fires(FaultRecv)) return -1;
    return __real_recv(fd, buffer, length, flags);
}

int __wrap_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
    if (fault_fires(FaultAccept)) return -1;
    return __real_accept4(fd, address, length, flags);
}

int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    if (fault_fires(FaultGetsockopt)) return -1;
    if (fake_peer_uid >= 0 && level == SOL_SOCKET && option == SO_PEERCRED) {
        struct ucred credentials = {.pid = getpid(), .uid = (uid_t)fake_peer_uid, .gid = getegid()};
        memcpy(value, &credentials, sizeof(credentials));
        *length = sizeof(credentials);
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, length);
}

int __wrap_epoll_create1(int flags)
{
    if (fault_fires(FaultEpollCreate)) return -1;
    return __real_epoll_create1(flags);
}

int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    if (fault_fires(FaultEpollCtl)) return -1;
    return __real_epoll_ctl(epfd, op, fd, event);
}

int __wrap_epoll_wait(int epfd, struct epoll_event *events, int maximum, int timeout)
{
    if (fault_fires(FaultEpollWait)) return -1;
    if (scripted_count >= 0) {
        int count = scripted_count;
        assert(count <= maximum);
        memcpy(events, scripted_events, (size_t)count * sizeof(*events));
        scripted_count = -1;
        return count;
    }
    return __real_epoll_wait(epfd, events, maximum, timeout);
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
    handler->stopping = 1;
    handler->stop_client_index = 0;
    handler->stop_client_serial = serial;
    handler->stop_request_id = 7;
    hold_stop_requester(handler);
    Check(handler->stop_reply_pending == 1 && handler->clients[0].fd == -1);
    /* The held descriptor is /dev/null here, so the post-cleanup send cannot succeed. */
    Check(session_local_handler_send_stop_reply(handler) == 0);
    Check(handler->stop_reply_pending == 0 && handler->stats.stop_deadline_expired == 1);
    Check(session_local_handler_send_stop_reply(handler) == 0);
    handler->stopping = 0;
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
    Check(handle_launch_result(handler, FakeSender, second, bare, sizeof(bare)) == SessionLocalCallbackFailure);
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
        if (fds[0].revents != 0) {
            int result = table->on_fds(table->context, fds, count, now_ms, FakeSender);
            assert(result == 0 || result == SessionHandlerRequestStop);
        }
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
    Check(table.on_ready(table.context, 0, FakeSender) != 0);

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
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
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
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
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
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
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
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
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
    Check(second_table.on_ready(second_table.context, 0, FakeSender) != 0);
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

    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

/* A refused connection gets one unsolicited Busy Error (request id 0), then end of stream. */
static void expect_busy_refusal(int fd)
{
    uint8_t reply[64];
    size_t received = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ssize_t got = recv(fd, reply + received, sizeof(reply) - received, 0);
        Check(got >= 0);
        if (got == 0) break;
        received += (size_t)got;
    }
    interop_header_t header;
    Check(received >= InteropHeaderBytes);
    Check(interop_header_decode(reply, received, &header) == 0);
    Check(header.type == InteropMsgError && header.request_id == 0);
    Check(received == InteropHeaderBytes + header.length);
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
    expect_busy_refusal(clients[SessionLocalMaxClients]);
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) Check(close(clients[index]) == 0);
    Check(close(clients[SessionLocalMaxClients]) == 0);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_local_stop_replies_then_requests_stop(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int client = connect_path(handler->socket_path);
    int other = connect_path(handler->socket_path);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_frame_with(frame, sizeof(frame), InteropMsgLocalStop, 21, InteropFromLocal);
    send_all(client, frame, length);
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 0;
    Check(table.poll_fds(table.context, fds, SessionHandlerMaxDescriptors, &count) == 0);
    int stop = 0;
    for (int round = 0; round < 3 && !stop; ++round) {
        (void)poll(fds, count, round == 0 ? 20 : 5);
        if (fds[0].revents != 0) stop = table.on_fds(table.context, fds, count, 2000, FakeSender) == SessionHandlerRequestStop;
    }
    Check(stop);
    Check(handler->stopping == 1 && handler->stats.stop_requests == 1);
    /* IMPL_DESC 3.12.5: nothing is sent to the requester until the session has cleaned up. */
    uint8_t byte;
    Check(recv(client, &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN);

    /* Input after the stop is never read, and a new launch is refused as busy. */
    length = build_local_launch(frame, sizeof(frame), 22, InteropLaunchPath, "Z:\\late.exe", NULL, NULL, NULL);
    send_all(other, frame, length);
    pump_once(&table, 2100);
    Check(fake_sends == 0);
    Check(handler->stats.accepted == 2);
    int late = connect_path(handler->socket_path);
    pump_once(&table, 2200);
    expect_busy_refusal(late);
    Check(close(late) == 0);
    Check(handler->stats.refused_capacity == 1);

    /* Cleanup holds the requester and cancels the rest; the reply follows once the owners are released. */
    table.on_closing(table.context, 0, FakeSender);
    Check(handler->stop_reply_pending == 1 && handler->clients[0].fd == -1);
    Check(session_local_handler_send_stop_reply(handler) == 1);
    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    Check(recv_frame(client, &reply, payload, &payload_length));
    Check(reply.type == InteropMsgLocalStopResult && reply.request_id == 21);
    Check(interop_payload_validate(reply.type, InteropFromLocal, payload, payload_length) == 0);
    uint64_t status = 9;
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropOk);
    Check(session_local_handler_send_stop_reply(handler) == 0);

    Check(close(client) == 0);
    Check(close(other) == 0);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_local_stop_flush_bound_and_lost_client(void)
{
    /* The post-cleanup send is a no-op without a held reply, and a held reply to a lost peer is
     * not written: the descriptor is closed and the failure counted. */
    fake_reset();
    session_local_handler_t *handler = new_handler();
    Check(session_local_handler_send_stop_reply(handler) == 0);
    Check(session_local_handler_send_stop_reply(NULL) == 0);
    uint64_t serial = attach_client(handler, 0);
    handler->stopping = 1;
    handler->stop_client_index = 0;
    handler->stop_client_serial = serial;
    handler->stop_request_id = 9;
    Check(stop_ready(handler, 0) == 1);
    hold_stop_requester(handler);
    Check(handler->stop_reply_pending == 1 && handler->clients[0].fd == -1);
    Check(session_local_handler_send_stop_reply(handler) == 0);
    Check(handler->stop_reply_pending == 0 && handler->stats.stop_deadline_expired == 1);
    handler->stopping = 0;
    free_handler(handler);
}

static void test_launch_timeout_and_late_result(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int client = connect_path(handler->socket_path);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 41, InteropLaunchPath, "Z:\\slow.exe", NULL, NULL, NULL);
    send_all(client, frame, length);
    pump_once(&table, 1000);
    Check(fake_sends == 1 && handler->pending[0].in_use == 1);
    Check(table.next_deadline_ms(table.context, 1000) == 1000 + SessionLocalLaunchTimeoutMs);
    uint32_t guest_id = handler->pending[0].guest_request_id;

    Check(table.on_tick(table.context, 30999, FakeSender) == 0);
    Check(handler->pending[0].in_use == 1 && handler->stats.timeouts == 0);
    Check(table.on_tick(table.context, 31000, FakeSender) == 0);
    Check(handler->stats.timeouts == 1 && handler->pending[0].in_use == 0);
    Check(fake_released_count == 1 && fake_released[0] == guest_id);
    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    Check(recv_frame(client, &reply, payload, &payload_length));
    uint64_t status = 0;
    Check(reply.type == InteropMsgLaunchResult && reply.request_id == 41);
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropTimedOut);

    uint8_t late[InteropMaxFrameBytes];
    size_t late_length = build_guest_result(late, sizeof(late), guest_id, InteropOk, 5, NULL, NULL);
    Check(table.on_frame(table.context, InteropMsgLaunchResult, guest_id, late, late_length, FakeSender) == 0);
    Check(handler->stats.ignored_results == 1 && fake_released_count == 1);
    Check(close(client) == 0);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_idle_limit_before_first_frame(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    Check(table.next_deadline_ms(table.context, 0) == UINT64_MAX);
    int idle = connect_path(handler->socket_path);
    pump_once(&table, 100);
    Check(handler->clients[0].fd >= 0 && handler->clients[0].frame_seen == 0);
    Check(table.next_deadline_ms(table.context, 100) == 100 + SessionLocalIdleTimeoutMs);
    Check(table.on_tick(table.context, 10099, FakeSender) == 0);
    Check(handler->clients[0].fd >= 0 && handler->stats.idle_closed == 0);
    Check(table.on_tick(table.context, 10100, FakeSender) == 0);
    Check(handler->clients[0].fd == -1 && handler->stats.idle_closed == 1);
    uint8_t byte;
    Check(recv(idle, &byte, 1, 0) == 0);
    Check(close(idle) == 0);

    /* A client that completed one frame is idle-closed 120 s after its last frame, with nothing pending. */
    int active = connect_path(handler->socket_path);
    pump_once(&table, 20000);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 51, InteropLaunchPath, "Z:\\i.exe", NULL, NULL, NULL);
    send_all(active, frame, length);
    pump_once(&table, 20001);
    Check(handler->clients[0].frame_seen == 1);
    Check(table.on_tick(table.context, 20001 + SessionLocalIdleAfterFrameMs - 1u, FakeSender) == 0);
    Check(handler->clients[0].fd >= 0 && handler->stats.idle_closed == 1);
    Check(table.on_tick(table.context, 20001 + SessionLocalIdleAfterFrameMs, FakeSender) == 0);
    Check(handler->clients[0].fd == -1 && handler->stats.idle_closed == 2);
    Check(close(active) == 0);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_on_closing_cancels_releases_and_unlinks(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    size_t descriptors_before = count_open_descriptors();
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int client = connect_path(handler->socket_path);
    int idle = connect_path(handler->socket_path);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 61, InteropLaunchPath, "Z:\\c.exe", NULL, NULL, NULL);
    send_all(client, frame, length);
    pump_once(&table, 500);
    Check(fake_sends == 1 && handler->pending[0].in_use == 1);
    uint32_t guest_id = handler->pending[0].guest_request_id;
    Check(table.next_deadline_ms(table.context, 500) != UINT64_MAX);

    table.on_closing(table.context, HostControlCancelledReason, FakeSender);
    Check(handler->closed == 1 && handler->bound == 0 && handler->socket_created == 0);
    Check(handler->pending[0].in_use == 0);
    Check(fake_released_count == 1 && fake_released[0] == guest_id);
    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    Check(recv_frame(client, &reply, payload, &payload_length));
    uint64_t status = 0;
    Check(reply.type == InteropMsgLaunchResult && reply.request_id == 61);
    Check(interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) == 0 && status == InteropCancelled);
    uint8_t byte;
    Check(recv(client, &byte, 1, 0) == 0);
    Check(close(client) == 0);
    Check(close(idle) == 0);

    /* Exactly once: a second notification changes nothing; the file is gone; no descriptor leaks. */
    table.on_closing(table.context, HostControlCancelledReason, FakeSender);
    Check(fake_released_count == 1);
    char path[256];
    Check(snprintf(path, sizeof(path), "%s/waddle/alpha/session.sock", test_runtime) > 0);
    mode_t mode = 0;
    Check(socket_path_mode(path, &mode) != 0);
    Check(count_open_descriptors() == descriptors_before);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_on_closing_before_ready_is_inert(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.next_deadline_ms(table.context, 0) == UINT64_MAX);
    table.on_closing(table.context, HostControlCancelledReason, FakeSender);
    Check(handler->closed == 1 && fake_released_count == 0);
    free_handler(handler);
}

/* ---- Fault paths, scripted events and direct state edges ---- */

static void test_init_path_length_and_names(void)
{
    session_local_handler_t *handler = calloc(1, sizeof(*handler));
    session_local_config_t config = {.runtime_dir = "/run/user/1000", .device_name = "A_b-9", .lock_held_proof = 0, .reserved_zero = 0};
    char runtime[4200];
    Check(handler != NULL);
    Check(session_local_handler_init(handler, &config) == SessionLocalSuccess);
    config.device_name = "x.y";
    Check(session_local_handler_init(handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "alpha";
    /* The socket path must fit sun_path (107 bytes plus NUL): sweep the boundary exactly. */
    for (size_t length = 80; length <= 100; ++length) {
        size_t prefix = length - 1u;
        memset(runtime, 'r', sizeof(runtime));
        runtime[0] = '/';
        runtime[prefix] = '\0';
        config.runtime_dir = runtime;
        size_t socket_length = prefix + strlen("/waddle/alpha/session.sock");
        int expected = socket_length < SessionLocalSocketPathBytes ? SessionLocalSuccess : SessionLocalInvalidArgument;
        Check(session_local_handler_init(handler, &config) == expected);
    }
    memset(runtime, 'r', 4096);
    runtime[0] = '/';
    runtime[4096] = '\0';
    config.runtime_dir = runtime;
    Check(session_local_handler_init(handler, &config) == SessionLocalInvalidArgument);
    runtime[4095] = '\0';
    Check(session_local_handler_init(handler, &config) == SessionLocalInvalidArgument);
    free(handler);
}

static void test_ready_fault_paths(void)
{
    static const struct {
        fault_point_t point;
        int nth;
        int error;
    } cases[] = {
        {FaultLstat, 1, EACCES}, {FaultMkdir, 1, EACCES}, {FaultLstat, 2, EIO},
        {FaultLstat, 3, EACCES}, {FaultMkdir, 2, EACCES}, {FaultLstat, 4, EIO},
        {FaultLstat, 5, EIO}, {FaultBind, 1, EADDRINUSE}, {FaultLstat, 6, EIO},
        {FaultListen, 1, EADDRINUSE}, {FaultEpollCreate, 1, ENOMEM}, {FaultEpollCtl, 1, ENOMEM},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        fake_reset();
        make_runtime();
        session_local_handler_t *handler = open_handler("alpha", 1);
        session_handler_t table;
        char path[256];
        mode_t mode = 0;
        session_local_handler_table(handler, &table);
        fault_reset();
        fault_arm(cases[index].point, cases[index].nth, cases[index].error);
        Check(table.on_ready(table.context, 0, FakeSender) != 0);
        Check(!handler->bound && !handler->socket_created);
        Check(handler->listen_fd == -1 && handler->epoll_fd == -1);
        Check(snprintf(path, sizeof(path), "%s/waddle/alpha/session.sock", test_runtime) > 0);
        Check(socket_path_mode(path, &mode) != 0);
        fault_reset();
        session_local_handler_destroy(handler);
        free(handler);
        remove_tree(test_runtime);
    }

    /* Stale socket path: the probe socket, the unlink, and a read error on the path all refuse. */
    make_runtime();
    char device[256];
    char path[256];
    Check(snprintf(device, sizeof(device), "%s/waddle", test_runtime) > 0);
    Check(mkdir(device, 0700) == 0);
    Check(snprintf(device, sizeof(device), "%s/waddle/alpha", test_runtime) > 0);
    Check(mkdir(device, 0700) == 0);
    Check(snprintf(path, sizeof(path), "%s/session.sock", device) > 0);
    int stale = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    Check(stale >= 0 && bind(stale, (const struct sockaddr *)&address, sizeof(address)) == 0);
    Check(close(stale) == 0);
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    fault_reset();
    fault_arm(FaultSocket, 1, EMFILE);
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
    fault_reset();
    fault_arm(FaultUnlink, 1, EACCES);
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
    mode_t mode = 0;
    Check(socket_path_mode(path, &mode) == 0 && S_ISSOCK(mode));
    fault_reset();
    fault_arm(FaultLstat, 3, EIO);
    Check(table.on_ready(table.context, 0, FakeSender) != 0);
    fault_reset();
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_accept_and_peer_fault_paths(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    fault_reset();

    int first = connect_path(handler->socket_path);
    fault_arm(FaultAccept, 1, EMFILE);
    /* The failed accept leaves the connection queued; the second round of the cycle accepts it. */
    pump_once(&table, 10);
    Check(handler->stats.accept_errors == 1 && handler->stats.accepted == 1);

    int second = connect_path(handler->socket_path);
    fault_arm(FaultAccept, 1, EINTR);
    pump_once(&table, 12);
    Check(handler->stats.accepted == 2 && handler->stats.accept_errors == 1);
    int third = connect_path(handler->socket_path);
    fault_arm(FaultAccept, 1, ECONNABORTED);
    pump_once(&table, 13);
    Check(handler->stats.accepted == 3 && handler->stats.accept_errors == 1);

    int refused = connect_path(handler->socket_path);
    fake_peer_uid = (int)geteuid() + 1;
    pump_once(&table, 14);
    Check(handler->stats.refused_peer == 1);
    fake_peer_uid = -1;
    uint8_t byte;
    Check(recv(refused, &byte, 1, 0) == 0);
    Check(close(refused) == 0);

    int unreadable = connect_path(handler->socket_path);
    fault_arm(FaultGetsockopt, 1, EPERM);
    pump_once(&table, 15);
    Check(handler->stats.refused_peer == 2);
    Check(recv(unreadable, &byte, 1, 0) == 0);
    Check(close(unreadable) == 0);

    handler->stopping = 1;
    int stopping = connect_path(handler->socket_path);
    pump_once(&table, 16);
    Check(handler->stats.refused_capacity == 1);
    expect_busy_refusal(stopping);
    Check(close(stopping) == 0);
    handler->stopping = 0;

    int broken = connect_path(handler->socket_path);
    fault_arm(FaultEpollCtl, 1, ENOMEM);
    pump_once(&table, 17);
    Check(handler->stats.closed_io == 1 && handler->stats.accepted == 3);
    Check(recv(broken, &byte, 1, 0) == 0);
    Check(close(broken) == 0);

    Check(close(first) == 0);
    Check(close(second) == 0);
    Check(close(third) == 0);
    fault_reset();
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_client_io_fault_paths(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    int client = connect_path(handler->socket_path);
    pump_once(&table, 100);
    Check(handler->clients[0].fd >= 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_local_launch(frame, sizeof(frame), 71, InteropLaunchPath, "Z:\\r.exe", NULL, NULL, NULL);

    /* Receive: EAGAIN and EINTR leave the bytes for the next cycle; a hard error closes the client. */
    send_all(client, frame, length);
    fault_arm(FaultRecv, 1, EAGAIN);
    read_client(handler, 0, 110, FakeSender);
    Check(fake_sends == 0 && handler->clients[0].fd >= 0 && handler->clients[0].rx_length == 0);
    fault_arm(FaultRecv, 1, EINTR);
    read_client(handler, 0, 111, FakeSender);
    Check(fake_sends == 0 && handler->clients[0].fd >= 0);
    read_client(handler, 0, 112, FakeSender);
    Check(fake_sends == 1);
    fault_arm(FaultRecv, 1, ECONNRESET);
    send_all(client, frame, length);
    read_client(handler, 0, 113, FakeSender);
    Check(handler->clients[0].fd == -1 && handler->stats.closed_io == 1);
    Check(close(client) == 0);

    /* Scripted epoll events: output, hang-up, unknown tags, and the listener tag. */
    client = connect_path(handler->socket_path);
    pump_once(&table, 120);
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 0;
    Check(table.poll_fds(table.context, fds, SessionHandlerMaxDescriptors, &count) == 0);
    fds[0].revents = POLLIN;
    scripted_count = 4;
    scripted_events[0].events = EPOLLOUT;
    scripted_events[0].data.u64 = 1;
    scripted_events[1].events = EPOLLIN;
    scripted_events[1].data.u64 = 99;
    scripted_events[2].events = EPOLLIN;
    scripted_events[2].data.u64 = 4;
    scripted_events[3].events = EPOLLIN;
    scripted_events[3].data.u64 = 0;
    Check(table.on_fds(table.context, fds, count, 121, FakeSender) == 0);
    Check(handler->clients[0].fd >= 0 && handler->stats.closed_io == 1);

    /* A hang-up while stopping closes the client (no input is read while stopping). */
    /* No stop client is attached here, so the stop is ready at once after the hang-up. */
    handler->stopping = 1;
    handler->stop_client_serial = 0;
    scripted_count = 1;
    scripted_events[0].events = EPOLLHUP;
    scripted_events[0].data.u64 = 1;
    Check(table.on_fds(table.context, fds, count, 122, FakeSender) == SessionHandlerRequestStop);
    Check(handler->clients[0].fd == -1 && handler->stats.closed_io == 2);
    handler->stopping = 0;
    Check(close(client) == 0);

    /* epoll wait: EINTR is not an error; any other errno fails the callback. */
    fault_arm(FaultEpollWait, 1, EINTR);
    Check(table.on_fds(table.context, fds, count, 130, FakeSender) == 0);
    fault_arm(FaultEpollWait, 1, EBADF);
    Check(table.on_fds(table.context, fds, count, 131, FakeSender) == SessionLocalCallbackFailure);
    fault_reset();

    /* Send: EAGAIN keeps the bytes and the writable interest; EINTR retries; EPIPE closes. */
    client = connect_path(handler->socket_path);
    pump_once(&table, 140);
    Check(reply_launch_result(handler, 0, handler->clients[0].serial, 72, InteropOk, 1, NULL, 0) == 1);
    fault_arm(FaultSend, 1, EAGAIN);
    Check(table.on_tick(table.context, 141, FakeSender) == 0);
    Check(handler->clients[0].tx_offset == 0 && handler->clients[0].tx_length > 0 && handler->clients[0].epoll_writable == 1);
    Check(table.on_tick(table.context, 142, FakeSender) == 0);
    Check(handler->clients[0].tx_length == 0 && handler->clients[0].epoll_writable == 0);
    interop_header_t reply;
    uint8_t payload[InteropMaxPayloadBytes];
    size_t payload_length = 0;
    Check(recv_frame(client, &reply, payload, &payload_length) && reply.request_id == 72);
    Check(reply_launch_result(handler, 0, handler->clients[0].serial, 73, InteropOk, 1, NULL, 0) == 1);
    fault_arm(FaultSend, 1, EINTR);
    Check(table.on_tick(table.context, 143, FakeSender) == 0);
    Check(recv_frame(client, &reply, payload, &payload_length) && reply.request_id == 73);
    Check(reply_launch_result(handler, 0, handler->clients[0].serial, 74, InteropOk, 1, NULL, 0) == 1);
    fault_arm(FaultSend, 1, EPIPE);
    Check(table.on_tick(table.context, 144, FakeSender) == 0);
    Check(handler->clients[0].fd == -1 && handler->stats.closed_io == 3);
    Check(close(client) == 0);

    /* Interest changes that fail leave the recorded interest unchanged. */
    client = connect_path(handler->socket_path);
    pump_once(&table, 150);
    Check(reply_launch_result(handler, 0, handler->clients[0].serial, 75, InteropOk, 1, NULL, 0) == 1);
    handler->clients[0].epoll_writable = 0;
    fault_arm(FaultEpollCtl, 1, EBADF);
    update_interest(handler, 0);
    Check(handler->clients[0].epoll_writable == 0);
    fault_reset();
    Check(close(client) == 0);
    pump_once(&table, 160);
    session_local_handler_destroy(handler);
    free(handler);
    remove_tree(test_runtime);
}

static void test_reply_and_state_edges(void)
{
    fake_reset();
    session_local_handler_t *handler = new_handler();
    session_handler_t table;
    session_local_handler_table(handler, &table);
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 7;

    /* Before ready and with bad arguments: no descriptors, inert ticks, refused polls. */
    Check(table.poll_fds(table.context, NULL, SessionHandlerMaxDescriptors, &count) != 0);
    Check(table.poll_fds(table.context, fds, 0, &count) != 0);
    Check(table.poll_fds(table.context, fds, SessionHandlerMaxDescriptors, &count) == 0 && count == 0);
    Check(table.on_fds(table.context, fds, 0, 1, FakeSender) == 0);
    Check(table.on_tick(table.context, 5, FakeSender) == 0 && handler->last_now_ms == 5);
    Check(table.on_frame(table.context, InteropMsgPong, 1, NULL, 0, FakeSender) == SessionLocalCallbackFailure);
    Check(table.next_deadline_ms(table.context, 0) == UINT64_MAX);

    /* A ready handler with a closed flag and no work: its callbacks stay inert. */
    handler->bound = 1;
    handler->closed = 1;
    Check(table.on_tick(table.context, 6, FakeSender) == 0);
    Check(table.on_fds(table.context, fds, 1, 6, FakeSender) == 0);
    Check(table.next_deadline_ms(table.context, 6) == UINT64_MAX);
    handler->closed = 0;
    handler->bound = 0;

    /* Stop deadline and launch deadlines compete for the earliest value. */
    uint64_t serial = attach_client(handler, 0);
    handler->bound = 1;
    handler->stopping = 1;
    handler->stop_client_index = 0;
    handler->stop_client_serial = serial;
    handler->stop_deadline_ms = 700;
    handler->pending[0].in_use = 1;
    handler->pending[0].deadline_ms = 900;
    Check(table.next_deadline_ms(table.context, 1) == 900);
    handler->stop_deadline_ms = 1000;
    Check(table.next_deadline_ms(table.context, 1) == 900);
    handler->stopping = 0;
    handler->pending[0].in_use = 0;

    /* A stop whose client stays but whose serial changed is ready at once. */
    handler->stopping = 1;
    handler->stop_client_serial = serial + 9;
    Check(stop_ready(handler, 0) == 1);
    handler->stopping = 0;

    /* Replies: a stale occupant gets nothing; a compacted buffer is reused; a full one closes. */
    Check(reply_launch_result(handler, 0, serial + 1, 1, InteropOk, 0, NULL, 0) == 0);
    Check(handler->stats.replies_dropped == 1);
    handler->clients[0].tx_offset = 3;
    handler->clients[0].tx_length = 3;
    Check(reply_launch_result(handler, 0, serial, 81, InteropOk, 1, NULL, 0) == 1);
    Check(handler->clients[0].tx_offset == 0 && handler->clients[0].tx_length > 0);
    handler->clients[0].tx_length = SessionLocalTxBytes - 4;
    handler->clients[0].tx_offset = 0;
    Check(reply_launch_result(handler, 0, serial, 82, InteropOk, 1, NULL, 0) == 0);
    Check(handler->clients[0].fd == -1 && handler->stats.closed_io == 1);

    /* An oversize launch message cannot be encoded; the client is closed rather than corrupted. */
    uint64_t second = attach_client(handler, 1);
    uint8_t long_message[1025];
    memset(long_message, 'm', sizeof(long_message));
    Check(reply_launch_result(handler, 1, second, 83, InteropOk, 1, long_message, sizeof(long_message)) == 0);
    Check(handler->clients[1].fd == -1 && handler->stats.closed_io == 2);
    Check(reply_error(handler, 1, second, 84, InteropErrorUnexpected, "gone") == 0);

    /* Idempotent close and direct helpers on a closed slot. */
    client_close(handler, 1);
    flush_client(handler, 1);
    update_interest(handler, 1);
    handler->epoll_fd = -1;
    handler->clients[0].fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    handler->clients[0].tx_length = 1;
    update_interest(handler, 0);
    Check(handler->clients[0].epoll_writable == 0);
    handler->clients[0].tx_length = 0;

    /* Receive buffer full without a complete frame is an overflow. */
    handler->clients[0].rx_length = SessionLocalRxBytes;
    read_client(handler, 0, 9, FakeSender);
    Check(handler->stats.closed_oversize == 1 && handler->clients[0].fd == -1);

    /* A launch whose pending entry belongs to a detached client is timed out silently. */
    fake_reset();
    uint64_t owner = attach_client(handler, 2);
    handler->pending[1].in_use = 1;
    handler->pending[1].guest_request_id = 500;
    handler->pending[1].local_request_id = 9;
    handler->pending[1].client_index = 2;
    handler->pending[1].client_serial = owner;
    handler->pending[1].deadline_ms = 10;
    detach_client(handler, 2);
    handler->bound = 1;
    expire_launches(handler, FakeSender, 20);
    Check(handler->stats.timeouts == 1 && fake_released_count == 1 && fake_released[0] == 500);
    Check(handler->pending[1].in_use == 0);
    handler->bound = 0;
    free_handler(handler);
}


static void test_frame_scan_rules(void)
{
    interop_header_t header;
    size_t frame_length = 99;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_frame_with(frame, sizeof(frame), InteropMsgLocalStop, 5, InteropFromLocal);
    Check(session_local_frame_scan(frame, 3, &header, &frame_length) == InteropLocalScanNeedMore && frame_length == 0);
    Check(session_local_frame_scan(frame, length, &header, &frame_length) == InteropLocalScanReady);
    Check(frame_length == length && header.type == InteropMsgLocalStop && header.request_id == 5);
    Check(session_local_frame_scan(frame, length - 1, &header, &frame_length) == InteropLocalScanNeedMore);

    /* Bytes after a complete frame are not part of it. */
    memset(frame + length, 0xAA, 8);
    Check(session_local_frame_scan(frame, length + 8, &header, &frame_length) == InteropLocalScanReady);
    Check(frame_length == length);

    /* Announced sizes: the local bound is checked before completeness, and the exact bound passes. */
    uint8_t oversize[InteropHeaderBytes];
    memcpy(oversize, frame, InteropHeaderBytes);
    oversize[8] = 0x00;
    oversize[9] = 0x10;
    oversize[10] = 0x00;
    oversize[11] = 0x00;
    Check(session_local_frame_scan(oversize, sizeof(oversize), &header, &frame_length) == InteropLocalScanTooLarge);
    Check(frame_length == 0);
    uint8_t exact[SessionLocalRxBytes];
    memset(exact, 0, sizeof(exact));
    memcpy(exact, frame, InteropHeaderBytes);
    exact[8] = (uint8_t)((SessionLocalRxBytes - InteropHeaderBytes) & 0xFF);
    exact[9] = (uint8_t)((SessionLocalRxBytes - InteropHeaderBytes) >> 8);
    Check(session_local_frame_scan(exact, sizeof(exact) - 1, &header, &frame_length) == InteropLocalScanNeedMore);
    Check(session_local_frame_scan(exact, sizeof(exact), &header, &frame_length) == InteropLocalScanReady);
    Check(frame_length == SessionLocalRxBytes);

    /* A bad magic, a bad version, and an unaligned length are malformed. */
    uint8_t bad[InteropHeaderBytes];
    memcpy(bad, frame, sizeof(bad));
    bad[0] ^= 0xFF;
    Check(session_local_frame_scan(bad, sizeof(bad), &header, &frame_length) == InteropLocalScanMalformed);
    memcpy(bad, frame, sizeof(bad));
    bad[4] ^= 0x01;
    Check(session_local_frame_scan(bad, sizeof(bad), &header, &frame_length) == InteropLocalScanMalformed);
    memcpy(bad, frame, sizeof(bad));
    bad[8] = 0x02;
    Check(session_local_frame_scan(bad, sizeof(bad), &header, &frame_length) == InteropLocalScanMalformed);
    Check(frame_length == 0);
}

/* Fill a path of exactly length bytes: a leading slash, then filler, with the terminating NUL. */
static void fill_path(char *buffer, size_t length)
{
    memset(buffer, 'r', length);
    buffer[0] = '/';
    buffer[length] = '\0';
}

/* Queue one reply of the requested kind to client slot 0 and return the builder's result. */
static int build_reply_kind(session_local_handler_t *handler, int kind, uint64_t serial)
{
    static const uint8_t message[] = "guest";
    switch (kind) {
    case 0: return reply_launch_result(handler, 0, serial, 7, InteropOk, 1234, message, sizeof(message) - 1u);
    case 1: return reply_error(handler, 0, serial, 7, InteropErrorUnexpected, "sweep");
    default: return reply_launch_result(handler, 0, serial, 7, InteropOk, 1, NULL, 0);
    }
}

/*
 * Every transmit room from zero up to one frame past the measured size: a frame that fits is
 * appended, and a frame that does not is refused with its client closed. This drives each
 * codec put in the reply builders to its failure edge.
 */
static void sweep_reply_room(session_local_handler_t *handler, int kind)
{
    uint64_t serial = attach_client(handler, 0);
    Check(build_reply_kind(handler, kind, serial) == 1);
    size_t frame_bytes = handler->clients[0].tx_length;
    Check(frame_bytes > (size_t)InteropHeaderBytes);
    client_close(handler, 0);

    for (size_t room = 0; room <= frame_bytes + 1u; ++room) {
        serial = attach_client(handler, 0);
        handler->clients[0].tx_length = (size_t)SessionLocalTxBytes - room;
        int sent = build_reply_kind(handler, kind, serial);
        if (room >= frame_bytes) {
            Check(sent == 1 && handler->clients[0].fd >= 0);
        } else {
            Check(sent == 0 && handler->clients[0].fd == -1);
        }
        client_close(handler, 0);
    }
}

static void test_branch_margin_edges(void)
{
    /* Init: each path stage that can overflow sun_path or the runtime cap refuses the handler. */
    static char path[SessionLocalRuntimeDirMax + 2];
    static session_local_handler_t scratch;
    session_local_config_t config = {.runtime_dir = path, .device_name = "alpha", .lock_held_proof = 0, .reserved_zero = 0};
    fill_path(path, SessionLocalRuntimeDirMax + 1u);
    Check(session_local_handler_init(&scratch, &config) == SessionLocalInvalidArgument);
    fill_path(path, 120);
    Check(session_local_handler_init(&scratch, &config) == SessionLocalInvalidArgument);
    fill_path(path, 101);
    Check(session_local_handler_init(&scratch, &config) == SessionLocalInvalidArgument);
    fill_path(path, 100);
    config.device_name = "a";
    Check(session_local_handler_init(&scratch, &config) == SessionLocalInvalidArgument);
    fill_path(path, 90);
    config.device_name = "wxyz";
    Check(session_local_handler_init(&scratch, &config) == SessionLocalInvalidArgument);
    fill_path(path, 80);
    config.device_name = "a";
    Check(session_local_handler_init(&scratch, &config) == 0);

    fake_reset();
    session_local_handler_t *handler = new_handler();
    session_handler_t table;
    session_local_handler_table(handler, &table);

    /* A ready attempt on a closed handler is refused without binding. */
    handler->closed = 1;
    Check(table.on_ready(table.context, 0, FakeSender) != 0 && handler->bound == 0);
    handler->closed = 0;

    /* Degenerate descriptor reporting: no count pointer, and a bound handler without an epoll
     * descriptor, and an empty descriptor request. */
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 0;
    Check(table.poll_fds(table.context, fds, 1, NULL) != 0);
    handler->bound = 1;
    Check(table.poll_fds(table.context, fds, 1, &count) == 0);
    Check(table.on_fds(table.context, fds, 0, 1, FakeSender) == 0);
    handler->bound = 0;

    /* The earliest pending launch deadline wins over a later one. */
    handler->bound = 1;
    handler->pending[0].in_use = 1;
    handler->pending[0].deadline_ms = 900;
    handler->pending[1].in_use = 1;
    handler->pending[1].deadline_ms = 950;
    Check(table.next_deadline_ms(table.context, 1) == 900);
    handler->pending[0].in_use = 0;
    handler->pending[1].in_use = 0;
    handler->bound = 0;

    /* Launches owned by another slot, or by the same slot in an older occupancy, are not counted. */
    uint64_t serial = attach_client(handler, 0);
    handler->pending[0].in_use = 1;
    handler->pending[0].client_index = 1;
    handler->pending[0].client_serial = serial;
    handler->pending[1].in_use = 1;
    handler->pending[1].client_index = 0;
    handler->pending[1].client_serial = serial + 5;
    Check(pending_outstanding(handler, 0, serial) == 0);
    handler->pending[0].in_use = 0;
    handler->pending[1].in_use = 0;

    /* A stopping handler leaves received bytes unprocessed. */
    handler->stopping = 1;
    handler->clients[0].rx_length = 16;
    (void)process_frames(handler, 0, 1, FakeSender);
    Check(handler->clients[0].fd >= 0 && handler->clients[0].rx_length == 16);
    handler->stopping = 0;
    client_close(handler, 0);

    /* Reply builders at every transmit room around one frame. */
    for (int kind = 0; kind < 3; ++kind) sweep_reply_room(handler, kind);
    free_handler(handler);
}

/* A closed handler binds again for a new session; a destroyed one never does. */
static void test_second_session_rearms_the_handler(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    table.on_closing(table.context, 0, FakeSender);
    Check(handler->closed == 1 && handler->bound == 0);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);
    Check(handler->bound == 1 && handler->closed == 0 && handler->stopping == 0);
    int client = connect_path(handler->socket_path);
    pump_once(&table, 200);
    Check(handler->stats.accepted == 1);
    Check(close(client) == 0);
    table.on_closing(table.context, 0, FakeSender);
    session_local_handler_destroy(handler);
    Check(table.on_ready(table.context, 0, FakeSender) != 0 && handler->bound == 0);
    free(handler);
    remove_tree(test_runtime);
}

/* A client that has sent a frame is closed after 120 s of silence, unless its launch is pending. */
static void test_idle_limit_after_a_frame(void)
{
    fake_reset();
    make_runtime();
    session_local_handler_t *handler = open_handler("alpha", 1);
    session_handler_t table;
    session_local_handler_table(handler, &table);
    Check(table.on_ready(table.context, 0, FakeSender) == 0);

    attach_client(handler, 0);
    handler->clients[0].frame_seen = 1;
    handler->clients[0].last_frame_ms = 1000;
    Check(table.on_tick(table.context, 1000 + SessionLocalIdleAfterFrameMs - 1u, FakeSender) == 0);
    Check(handler->clients[0].fd >= 0);
    Check(table.on_tick(table.context, 1000 + SessionLocalIdleAfterFrameMs, FakeSender) == 0);
    Check(handler->clients[0].fd == -1 && handler->stats.idle_closed == 1);

    /* A pending launch of the client keeps it open past the limit. */
    uint64_t serial = attach_client(handler, 1);
    handler->clients[1].frame_seen = 1;
    handler->clients[1].last_frame_ms = 1000;
    handler->pending[0].in_use = 1;
    handler->pending[0].client_index = 1;
    handler->pending[0].client_serial = serial;
    handler->pending[0].deadline_ms = UINT64_MAX;
    Check(table.on_tick(table.context, 1000 + 10u * SessionLocalIdleAfterFrameMs, FakeSender) == 0);
    Check(handler->clients[1].fd >= 0 && handler->stats.idle_closed == 1);
    handler->pending[0].in_use = 0;
    client_close(handler, 1);
    table.on_closing(table.context, 0, FakeSender);
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
    test_frame_scan_rules();
    test_client_launch_roundtrip_and_token();
    test_client_protocol_violations_close();
    test_max_clients_and_flush_order();
    test_local_stop_replies_then_requests_stop();
    test_local_stop_flush_bound_and_lost_client();
    test_launch_timeout_and_late_result();
    test_idle_limit_before_first_frame();
    test_on_closing_cancels_releases_and_unlinks();
    test_on_closing_before_ready_is_inert();
    test_init_path_length_and_names();
    test_ready_fault_paths();
    test_accept_and_peer_fault_paths();
    test_client_io_fault_paths();
    test_reply_and_state_edges();
    test_branch_margin_edges();
    test_second_session_rearms_the_handler();
    test_idle_limit_after_a_frame();
    Check(count_open_descriptors() == descriptors_before);
    printf("session_local_handler_test: passed\n");
    return 0;
}
