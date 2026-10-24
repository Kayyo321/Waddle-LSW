/**
 * @file test_guest_ready_client.c
 * @brief Socketpair tests for the bounded GuestReady client transaction.
 */
#include "daemon_client.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef enum fixture_reply_kind_t {
    FixtureReplyReady = 0,
    FixtureReplyError,
    FixtureReplyWrongSequence,
    FixtureReplyMalformedPayload,
    FixtureReplyEof,
    FixtureReplyDelay
} fixture_reply_kind_t;

typedef struct fixture_server_t {
    int fd;
    fixture_reply_kind_t kind;
} fixture_server_t;

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

static int transfer_all(int fd, uint8_t *bytes, size_t count, int writing) {
    size_t offset = 0;
    while (offset < count) {
        ssize_t moved = writing ? send(fd, bytes + offset, count - offset, MSG_NOSIGNAL) :
            recv(fd, bytes + offset, count - offset, 0);
        if (moved <= 0) return -1;
        offset += (size_t)moved;
    }
    return 0;
}

static void make_header(uint8_t *bytes, uint16_t type, uint32_t sequence, uint32_t payload_len) {
    put_u32(bytes, WaddleDaemonMagic);
    put_u16(bytes + 4, WaddleDaemonVersion);
    put_u16(bytes + 6, type);
    put_u32(bytes + 8, sequence);
    put_u32(bytes + 12, payload_len);
}

static void *fixture_server_main(void *opaque) {
    fixture_server_t *fixture = opaque;
    uint8_t request[WaddleDaemonHeaderSize + 4];
    assert(transfer_all(fixture->fd, request, sizeof(request), 0) == 0);
    assert(get_u32(request) == WaddleDaemonMagic);
    assert(request[6] == DaemonMsgGuestReadyReq && request[7] == 0);
    assert(get_u32(request + 12) == 4);
    assert(get_u32(request + WaddleDaemonHeaderSize) == 321);

    if (fixture->kind == FixtureReplyEof) {
        close(fixture->fd);
        return NULL;
    }
    if (fixture->kind == FixtureReplyDelay) {
        struct timespec delay = {.tv_sec = 2, .tv_nsec = 500000000};
        (void)nanosleep(&delay, NULL);
        close(fixture->fd);
        return NULL;
    }

    uint8_t frame[WaddleDaemonHeaderSize + 264] = {0};
    uint16_t type = DaemonMsgGuestReadyResp;
    uint32_t payload_len = 8;
    uint32_t sequence = get_u32(request + 8);
    if (fixture->kind == FixtureReplyError) {
        type = DaemonMsgErrorResp;
        payload_len = 264;
        put_u32(frame + WaddleDaemonHeaderSize, ENOTSUP);
    } else {
        if (fixture->kind == FixtureReplyWrongSequence) sequence++;
        put_u32(frame + WaddleDaemonHeaderSize, 0);
        put_u32(frame + WaddleDaemonHeaderSize + 4,
                fixture->kind == FixtureReplyMalformedPayload ? 2 : 33);
    }
    make_header(frame, type, sequence, payload_len);
    size_t frame_len = WaddleDaemonHeaderSize + payload_len;
    if (fixture->kind == FixtureReplyReady) {
        /* Fragment the response into single-byte writes. */
        for (size_t index = 0; index < frame_len; index++)
            assert(send(fixture->fd, frame + index, 1, MSG_NOSIGNAL) == 1);
    } else {
        /* One write coalesces the complete legacy ErrorResp. */
        assert(transfer_all(fixture->fd, frame, frame_len, 1) == 0);
    }
    close(fixture->fd);
    return NULL;
}

static int run_transaction(fixture_reply_kind_t kind, waddle_daemon_guest_ready_resp_t *response) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    fixture_server_t fixture = {.fd = pair[1], .kind = kind};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fixture_server_main, &fixture) == 0);
    int result = waddle_client_guest_ready(pair[0], 321, response);
    int saved_errno = errno;
    assert(pthread_join(thread, NULL) == 0);
    assert(close(pair[0]) == 0);
    errno = saved_errno;
    return result;
}

static void test_fragmented_success_and_correlated_sequence(void) {
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    assert(run_transaction(FixtureReplyReady, &response) == 0);
    assert(response.status == 0 && response.cid == 33);
}

static void test_old_daemon_error_and_malformed_correlation_preserve_output(void) {
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(run_transaction(FixtureReplyError, &response) == -1 && errno == ENOTSUP);
    assert(response.status == 91 && response.cid == 92);
    errno = 0;
    assert(run_transaction(FixtureReplyWrongSequence, &response) == -1 && errno == EPROTO);
    assert(response.status == 91 && response.cid == 92);
    errno = 0;
    assert(run_transaction(FixtureReplyMalformedPayload, &response) == -1 && errno == EPROTO);
    assert(response.status == 91 && response.cid == 92);
}

static void test_eof_and_invalid_arguments_preserve_output_and_borrowed_fd(void) {
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(run_transaction(FixtureReplyEof, &response) == -1 && errno == ECONNRESET);
    assert(response.status == 91 && response.cid == 92);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    errno = 0;
    assert(waddle_client_guest_ready(pair[0], 120001, &response) == -1 && errno == EINVAL);
    assert(response.status == 91 && response.cid == 92);
    assert(send(pair[1], "x", 1, MSG_NOSIGNAL) == 1);
    char byte = 0;
    assert(recv(pair[0], &byte, 1, 0) == 1 && byte == 'x');
    close(pair[0]);
    close(pair[1]);
}

static void test_total_deadline_covers_response_read(void) {
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(run_transaction(FixtureReplyDelay, &response) == -1 && errno == ETIMEDOUT);
    assert(response.status == 91 && response.cid == 92);
}

int main(void) {
    test_fragmented_success_and_correlated_sequence();
    test_old_daemon_error_and_malformed_correlation_preserve_output();
    test_eof_and_invalid_arguments_preserve_output_and_borrowed_fd();
    test_total_deadline_covers_response_read();
    puts("test_guest_ready_client: all tests passed");
    return 0;
}
