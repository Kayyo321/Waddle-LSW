/**
 * @file test_guest_ready_runtime.c
 * @brief Isolated AF_UNIX runtime tests for GuestReady framing and concurrency.
 */
#include "daemon_guest_ready.h"
#include "daemon_server.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct server_context_t {
    char runtime_dir[256];
    volatile sig_atomic_t stop_flag;
    pthread_t thread;
} server_context_t;

static atomic_int fail_server_clock;
static atomic_uint isolated_probe_attempts;
int __real_socket(int domain, int type, int protocol);
int __wrap_socket(int domain, int type, int protocol) {
    if (domain == AF_VSOCK) {
        atomic_fetch_add_explicit(&isolated_probe_attempts, 1, memory_order_relaxed);
        errno = EAFNOSUPPORT;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}
int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value) {
    if (atomic_exchange_explicit(&fail_server_clock, 0, memory_order_relaxed)) {
        errno = EIO;
        return -1;
    }
    return __real_clock_gettime(clock_id, value);
}

static void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; index++) bytes[index] = (uint8_t)(value >> (8 * index));
}

static uint32_t get_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void make_header(uint8_t *bytes, uint16_t type, uint32_t sequence, uint32_t payload_len) {
    put_u32(bytes, WaddleDaemonMagic);
    put_u16(bytes + 4, WaddleDaemonVersion);
    put_u16(bytes + 6, type);
    put_u32(bytes + 8, sequence);
    put_u32(bytes + 12, payload_len);
}

static void *server_main(void *opaque) {
    server_context_t *context = opaque;
    (void)daemon_server_run(context->runtime_dir, &context->stop_flag);
    return NULL;
}

static int connect_daemon(const char *path) {
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(fd >= 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        assert(strlen(path) < sizeof(address.sun_path));
        memcpy(address.sun_path, path, strlen(path) + 1);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) return fd;
        int saved_errno = errno;
        close(fd);
        assert(saved_errno == ENOENT || saved_errno == ECONNREFUSED);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000};
        (void)nanosleep(&pause, NULL);
    }
    assert(!"daemon socket did not become available");
    return -1;
}

static void send_bytes(int fd, const uint8_t *bytes, size_t count) {
    size_t offset = 0;
    while (offset < count) {
        ssize_t written = send(fd, bytes + offset, count - offset, MSG_NOSIGNAL);
        assert(written > 0);
        offset += (size_t)written;
    }
}

static uint64_t fixture_monotonic_ms(void) {
    struct timespec value;
    assert(__real_clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static int receive_frame(int fd, uint8_t *frame, size_t capacity, size_t *out_length,
                         int timeout_ms) {
    assert(capacity >= WaddleDaemonHeaderSize && timeout_ms > 0);
    uint64_t deadline = fixture_monotonic_ms() + (uint32_t)timeout_ms;
    size_t offset = 0, needed = WaddleDaemonHeaderSize;
    while (offset < needed) {
        uint64_t now = fixture_monotonic_ms();
        if (now >= deadline) return 0;
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return ready;
        ssize_t count = recv(fd, frame + offset, needed - offset, MSG_DONTWAIT);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
        if (offset == WaddleDaemonHeaderSize) {
            uint32_t payload_len = get_u32(frame + 12);
            if (payload_len > capacity - WaddleDaemonHeaderSize) return -1;
            needed += payload_len;
        }
    }
    *out_length = offset;
    return 1;
}

static void test_partial_response_has_one_absolute_deadline(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    const uint8_t partial[] = {1, 2, 3};
    send_bytes(pair[0], partial, sizeof(partial));
    uint8_t response[32];
    size_t length = 77;
    assert(receive_frame(pair[1], response, sizeof(response), &length, 20) == 0);
    assert(length == 77);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
}

static void send_request(int fd, uint16_t type, uint32_t sequence,
                         const uint8_t *payload, uint32_t payload_len) {
    uint8_t frame[WaddleDaemonHeaderSize + 8];
    assert(payload_len <= 8);
    make_header(frame, type, sequence, payload_len);
    if (payload_len != 0) memcpy(frame + WaddleDaemonHeaderSize, payload, payload_len);
    send_bytes(fd, frame, WaddleDaemonHeaderSize + payload_len);
}

static void test_fragmented_guest_ready_and_two_client_responsiveness(void) {
    server_context_t context = {0};
    char directory[] = "/tmp/waddle_guest_ready_XXXXXX";
    assert(mkdtemp(directory) != NULL);
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
    snprintf(context.runtime_dir, sizeof(context.runtime_dir), "%s/run", directory);
    assert(mkdir(context.runtime_dir, 0700) == 0);
    assert(setenv("WADDLE_MOCK_GUEST_SOCK", "/dev/null", 1) == 0);
    assert(pthread_create(&context.thread, NULL, server_main, &context) == 0);

    char socket_path[320];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", context.runtime_dir) <
           (int)sizeof(socket_path));
    int waiting_client = connect_daemon(socket_path);
    int observer_client = connect_daemon(socket_path);

    /* Mock startup reaches Running without launching QEMU or probing hardware. */
    uint8_t start_payload[8] = {0};
    send_request(waiting_client, DaemonMsgStartReq, 41, start_payload, sizeof(start_payload));
    uint8_t reply[WaddleDaemonHeaderSize + WaddleDaemonMaxPayloadSize];
    size_t reply_length = 0;
    assert(receive_frame(waiting_client, reply, sizeof(reply), &reply_length, 2000) == 1);
    assert(get_u32(reply + 8) == 41 && (reply[6] | (uint16_t)reply[7] << 8) == DaemonMsgStartResp);
    assert(get_u32(reply + WaddleDaemonHeaderSize) == 0);
    assert(get_u32(reply + WaddleDaemonHeaderSize + 4) == SubsystemStateRunning);

    /* Send GuestReady in fragments. The wait must not block another client. */
    uint8_t request[WaddleDaemonHeaderSize + 4];
    make_header(request, DaemonMsgGuestReadyReq, 0x10203040, 4);
    put_u32(request + WaddleDaemonHeaderSize, 700);
    send_bytes(waiting_client, request, 3);
    send_bytes(waiting_client, request + 3, WaddleDaemonHeaderSize - 3);
    send_bytes(waiting_client, request + WaddleDaemonHeaderSize, 2);
    uint8_t coalesced_status[WaddleDaemonHeaderSize];
    make_header(coalesced_status, DaemonMsgStatusReq, 42, 0);
    uint8_t tail[2 + WaddleDaemonHeaderSize];
    memcpy(tail, request + WaddleDaemonHeaderSize + 2, 2);
    memcpy(tail + 2, coalesced_status, sizeof(coalesced_status));
    send_bytes(waiting_client, tail, sizeof(tail));

    send_request(observer_client, DaemonMsgStatusReq, 77, NULL, 0);
    assert(receive_frame(observer_client, reply, sizeof(reply), &reply_length, 500) == 1);
    assert((reply[6] | (uint16_t)reply[7] << 8) == DaemonMsgStatusResp);
    assert(get_u32(reply + 8) == 77);
    assert(get_u32(reply + WaddleDaemonHeaderSize) == SubsystemStateRunning);
    assert(receive_frame(waiting_client, reply, sizeof(reply), &reply_length, 20) == 0);

    assert(receive_frame(waiting_client, reply, sizeof(reply), &reply_length, 2500) == 1);
    assert((reply[6] | (uint16_t)reply[7] << 8) == DaemonMsgGuestReadyResp);
    assert(get_u32(reply + 8) == 0x10203040);
    assert(get_u32(reply + 12) == 8 && reply_length == WaddleDaemonHeaderSize + 8);
    int32_t status = (int32_t)get_u32(reply + WaddleDaemonHeaderSize);
    uint32_t cid = get_u32(reply + WaddleDaemonHeaderSize + 4);
    assert(status == -ETIMEDOUT);
    assert(atomic_load_explicit(&isolated_probe_attempts, memory_order_relaxed) > 0);
    assert(cid >= 3 && cid != UINT32_MAX);
    assert(receive_frame(waiting_client, reply, sizeof(reply), &reply_length, 500) == 1);
    assert((reply[6] | (uint16_t)reply[7] << 8) == DaemonMsgStatusResp);
    assert(get_u32(reply + 8) == 42);

    send_request(observer_client, 0x7777, 78, NULL, 0);
    assert(receive_frame(observer_client, reply, sizeof(reply), &reply_length, 500) == 1);
    assert((reply[6] | (uint16_t)reply[7] << 8) == DaemonMsgErrorResp);
    assert(get_u32(reply + 8) == 78);
    assert(get_u32(reply + WaddleDaemonHeaderSize) == ENOTSUP);

    /* A readiness clock failure cancels clients but keeps the supervisor alive. */
    atomic_store_explicit(&fail_server_clock, 1, memory_order_relaxed);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 400000000};
    assert(nanosleep(&pause, NULL) == 0);
    int recovered_client = connect_daemon(socket_path);
    send_request(recovered_client, DaemonMsgStatusReq, 79, NULL, 0);
    assert(receive_frame(recovered_client, reply, sizeof(reply), &reply_length, 1000) == 1);
    assert(get_u32(reply + 8) == 79);
    assert(get_u32(reply + WaddleDaemonHeaderSize) == SubsystemStateRunning);
    send_request(recovered_client, DaemonMsgStopReq, 80, start_payload, sizeof(start_payload));
    assert(receive_frame(recovered_client, reply, sizeof(reply), &reply_length, 1000) == 1);
    assert(get_u32(reply + WaddleDaemonHeaderSize) == 0);
    send_request(recovered_client, DaemonMsgShutdownReq, 81, NULL, 0);
    assert(receive_frame(recovered_client, reply, sizeof(reply), &reply_length, 1000) == 1);
    assert(get_u32(reply + 8) == 81 && get_u32(reply + WaddleDaemonHeaderSize) == 0);
    assert(close(recovered_client) == 0);

    close(waiting_client);
    close(observer_client);
    assert(pthread_join(context.thread, NULL) == 0);
    assert(access(socket_path, F_OK) != 0);
    assert(unsetenv("WADDLE_MOCK_GUEST_SOCK") == 0);
    char cleanup[400];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory) < (int)sizeof(cleanup));
    assert(system(cleanup) == 0);
}

static void test_probe_deadline_wins_over_late_pollout(void) {
    daemon_guest_ready_t probe;
    daemon_guest_ready_init(&probe);
    assert(daemon_guest_ready_start(&probe, 3, 5242, 5000, 100, 9) == 0);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    probe.probe_fd = pair[0];
    probe.probe_deadline_ms = 2100;
    waddle_daemon_guest_ready_resp_t response = {.status = 123, .cid = 456};
    assert(daemon_guest_ready_step(&probe, 1, 2100, POLLOUT, &response) == 0);
    assert(probe.active && probe.probe_fd == -1);
    assert(response.status == 123 && response.cid == 456);
    assert(close(pair[1]) == 0);
    daemon_guest_ready_cancel(&probe);
}

int main(void) {
    test_partial_response_has_one_absolute_deadline();
    test_probe_deadline_wins_over_late_pollout();
    test_fragmented_guest_ready_and_two_client_responsiveness();
    puts("test_guest_ready_runtime: all tests passed");
    return 0;
}
