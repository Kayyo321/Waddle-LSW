/** @file test_guest_ready_client_faults.c
 * @brief Deterministic syscall-fault tests for the real GuestReady client.
 */
#include "daemon_client.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct io_action_t {
    ssize_t result;
    int error;
} io_action_t;

typedef struct poll_action_t {
    int result;
    int error;
    short revents;
} poll_action_t;

_Static_assert(sizeof(waddle_daemon_guest_ready_resp_t) == 8,
               "GuestReady response output must have no untested padding");

typedef enum clock_mode_t {
    ClockModeNormal = 0,
    ClockModeFailure,
    ClockModeNegative,
    ClockModeNegativeNanoseconds,
    ClockModeInvalidNanoseconds,
    ClockModeOverflow,
    ClockModeScripted
} clock_mode_t;

enum {
    MaxActions = 32,
    MaxReplyBytes = WaddleDaemonHeaderSize + 264,
    InitialSequence = 0x47520000
};

static struct {
    int enabled;
    clock_mode_t clock_mode;
    uint64_t clock_values[MaxActions];
    size_t clock_count;
    size_t clock_index;
    size_t clock_calls;
    size_t clock_fail_at_index;
    uint64_t normal_clock_ms;
    io_action_t send_actions[MaxActions];
    size_t send_action_count;
    size_t send_action_index;
    io_action_t recv_actions[MaxActions];
    size_t recv_action_count;
    size_t recv_action_index;
    poll_action_t poll_actions[MaxActions];
    size_t poll_action_count;
    size_t poll_action_index;
    uint8_t reply[MaxReplyBytes];
    size_t reply_length;
    size_t reply_offset;
    int reply_sequence_patched;
    uint32_t reply_sequence_delta;
    uint8_t request[WaddleDaemonHeaderSize + 4];
    size_t request_length;
} fault_state;

int __real_clock_gettime(clockid_t clock_id, struct timespec *value);
ssize_t __real_send(int fd, const void *bytes, size_t length, int flags);
ssize_t __real_recv(int fd, void *bytes, size_t length, int flags);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
static uint32_t get_u32(const uint8_t *bytes);
static void put_u32(uint8_t *bytes, uint32_t value);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value) {
    if (!fault_state.enabled) return __real_clock_gettime(clock_id, value);
    if (fault_state.clock_calls++ == fault_state.clock_fail_at_index) { errno = EIO; return -1; }
    if (fault_state.clock_mode == ClockModeFailure) { errno = EIO; return -1; }
    if (fault_state.clock_mode == ClockModeNegative) { value->tv_sec = -1; value->tv_nsec = 0; return 0; }
    if (fault_state.clock_mode == ClockModeNegativeNanoseconds) {
        value->tv_sec = 1;
        value->tv_nsec = -1;
        return 0;
    }
    if (fault_state.clock_mode == ClockModeInvalidNanoseconds) {
        value->tv_sec = 1;
        value->tv_nsec = 1000000000L;
        return 0;
    }
    uint64_t milliseconds = fault_state.normal_clock_ms;
    if (fault_state.clock_mode == ClockModeOverflow) {
        value->tv_sec = (time_t)(UINT64_MAX / 1000);
        value->tv_nsec = 0;
        return 0;
    }
    if (fault_state.clock_mode == ClockModeScripted) {
        assert(fault_state.clock_index < fault_state.clock_count);
        milliseconds = fault_state.clock_values[fault_state.clock_index++];
    } else {
        fault_state.normal_clock_ms++;
    }
    value->tv_sec = (time_t)(milliseconds / 1000);
    value->tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    return 0;
}

static ssize_t take_io_action(io_action_t *actions, size_t count, size_t *index,
                              size_t requested) {
    if (*index >= count) return (ssize_t)requested;
    io_action_t action = actions[(*index)++];
    if (action.result < 0) { errno = action.error; return -1; }
    if ((size_t)action.result > requested) return (ssize_t)requested;
    return action.result;
}

ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags) {
    (void)fd;
    (void)flags;
    if (!fault_state.enabled) return __real_send(fd, bytes, length, flags);
    ssize_t amount = take_io_action(fault_state.send_actions, fault_state.send_action_count,
                                    &fault_state.send_action_index, length);
    if (amount > 0) {
        assert(fault_state.request_length + (size_t)amount <= sizeof(fault_state.request));
        memcpy(fault_state.request + fault_state.request_length, bytes, (size_t)amount);
        fault_state.request_length += (size_t)amount;
    }
    return amount;
}

static void patch_reply_sequence(void) {
    if (fault_state.reply_sequence_patched || fault_state.request_length < WaddleDaemonHeaderSize) return;
    put_u32(fault_state.reply + 8, get_u32(fault_state.request + 8) + fault_state.reply_sequence_delta);
    fault_state.reply_sequence_patched = 1;
}

ssize_t __wrap_recv(int fd, void *bytes, size_t length, int flags) {
    (void)fd;
    (void)flags;
    if (!fault_state.enabled) return __real_recv(fd, bytes, length, flags);
    ssize_t action = take_io_action(fault_state.recv_actions, fault_state.recv_action_count,
                                    &fault_state.recv_action_index, length);
    if (action <= 0) return action;
    patch_reply_sequence();
    size_t available = fault_state.reply_length - fault_state.reply_offset;
    size_t amount = (size_t)action < available ? (size_t)action : available;
    if (amount == 0) return 0;
    memcpy(bytes, fault_state.reply + fault_state.reply_offset, amount);
    fault_state.reply_offset += amount;
    return (ssize_t)amount;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    (void)timeout;
    if (!fault_state.enabled) return __real_poll(fds, count, timeout);
    assert(count == 1);
    assert(fault_state.poll_action_index < fault_state.poll_action_count);
    poll_action_t action = fault_state.poll_actions[fault_state.poll_action_index++];
    if (action.result < 0) errno = action.error;
    if (action.result > 0) fds[0].revents = action.revents;
    return action.result;
}

static void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[index] = (uint8_t)(value >> (index * 8));
}

static uint32_t get_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

typedef enum reply_kind_t {
    ReplyReady = 0,
    ReplyError,
    ReplyBadMagic,
    ReplyBadVersion,
    ReplyOversizedHeader,
    ReplyWrongType,
    ReplyWrongSequence,
    ReplyWrongLength,
    ReplyErrorWrongLength,
    ReplyBadReadyPayload,
    ReplyBadErrorZero,
    ReplyBadErrorTooLarge
} reply_kind_t;

static void make_reply(reply_kind_t kind) {
    memset(fault_state.reply, 0, sizeof(fault_state.reply));
    uint16_t type = DaemonMsgGuestReadyResp;
    uint32_t payload_length = 8;
    if (kind == ReplyError || kind == ReplyErrorWrongLength ||
        kind == ReplyBadErrorZero || kind == ReplyBadErrorTooLarge) {
        type = DaemonMsgErrorResp;
        payload_length = 264;
        put_u32(fault_state.reply + WaddleDaemonHeaderSize,
                kind == ReplyBadErrorZero ? 0 : kind == ReplyBadErrorTooLarge ? 4096 : ENOTSUP);
    } else {
        put_u32(fault_state.reply + WaddleDaemonHeaderSize, 0);
        put_u32(fault_state.reply + WaddleDaemonHeaderSize + 4,
                kind == ReplyBadReadyPayload ? 2 : 33);
    }
    put_u32(fault_state.reply, kind == ReplyBadMagic ? 0 : WaddleDaemonMagic);
    put_u16(fault_state.reply + 4, kind == ReplyBadVersion ? (uint16_t)(WaddleDaemonVersion + 1) : WaddleDaemonVersion);
    put_u16(fault_state.reply + 6, kind == ReplyWrongType ? DaemonMsgStatusResp : type);
    put_u32(fault_state.reply + 8, InitialSequence + (kind == ReplyWrongSequence ? 1 : 0));
    fault_state.reply_sequence_delta = kind == ReplyWrongSequence ? 1 : 0;
    put_u32(fault_state.reply + 12,
            kind == ReplyOversizedHeader ? WaddleDaemonMaxPayloadSize + 1 :
            kind == ReplyWrongLength ? 7 : kind == ReplyErrorWrongLength ? 263 : payload_length);
    fault_state.reply_length = WaddleDaemonHeaderSize + payload_length;
}

static void reset_fault_state(reply_kind_t kind) {
    memset(&fault_state, 0, sizeof(fault_state));
    fault_state.enabled = 1;
    fault_state.clock_mode = ClockModeNormal;
    fault_state.clock_fail_at_index = SIZE_MAX;
    fault_state.normal_clock_ms = 1000;
    make_reply(kind);
}

static int run_client(int fd, waddle_daemon_guest_ready_resp_t *response) {
    return waddle_client_guest_ready(fd, 321, response);
}

static int run_client_timeout(int fd, uint32_t timeout_ms,
                              waddle_daemon_guest_ready_resp_t *response) {
    return waddle_client_guest_ready(fd, timeout_ms, response);
}

static void assert_response_unchanged(const waddle_daemon_guest_ready_resp_t *response) {
    const waddle_daemon_guest_ready_resp_t expected = {.status = 91, .cid = 92};
    assert(memcmp(response, &expected, sizeof(expected)) == 0);
}

static void test_valid_reply_and_sequence_from_actual_request(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyReady);
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    assert(run_client(pair[0], &response) == 0);
    assert(response.status == 0 && response.cid == 33);
    assert(fault_state.request_length == WaddleDaemonHeaderSize + 4);
    assert(get_u32(fault_state.request + 8) == InitialSequence);
    assert(get_u32(fault_state.request + WaddleDaemonHeaderSize) == 321);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);
}

static void test_zero_timeout_selects_default_deadline(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyReady);
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    assert(run_client_timeout(pair[0], 0, &response) == 0);
    assert(response.status == 0 && response.cid == 33);
    assert(get_u32(fault_state.request + WaddleDaemonHeaderSize) == 0);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);
}

static void test_short_io_eintr_eagain_poll_eintr_and_readiness(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyReady);
    fault_state.send_actions[0] = (io_action_t){.result = -1, .error = EINTR};
    fault_state.send_actions[1] = (io_action_t){.result = -1, .error = EAGAIN};
    fault_state.send_actions[2] = (io_action_t){.result = -1, .error = EAGAIN};
    fault_state.send_actions[3] = (io_action_t){.result = 3};
    fault_state.send_actions[4] = (io_action_t){.result = 17};
    fault_state.send_action_count = 5;
    fault_state.poll_actions[0] = (poll_action_t){.result = -1, .error = EINTR};
    fault_state.poll_actions[1] = (poll_action_t){.result = 1, .revents = POLLOUT};
    fault_state.poll_action_count = 2;
    fault_state.recv_actions[0] = (io_action_t){.result = -1, .error = EINTR};
    fault_state.recv_actions[1] = (io_action_t){.result = -1, .error = EWOULDBLOCK};
    fault_state.recv_actions[2] = (io_action_t){.result = -1, .error = EAGAIN};
    fault_state.recv_actions[3] = (io_action_t){.result = 4};
    fault_state.recv_actions[4] = (io_action_t){.result = 12};
    fault_state.recv_action_count = 5;
    fault_state.poll_actions[2] = (poll_action_t){.result = -1, .error = EINTR};
    fault_state.poll_actions[3] = (poll_action_t){.result = 1, .revents = POLLIN};
    fault_state.poll_action_count = 4;
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    assert(run_client(pair[0], &response) == 0);
    assert(response.status == 0 && response.cid == 33);
    assert(fault_state.poll_action_index == 4);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);
}

static void test_eagain_poll_failures_and_clock_errors(void) {
    const poll_action_t poll_failures[] = {
        {.result = 0}, {.result = 1, .revents = POLLNVAL}, {.result = -1, .error = EIO}
    };
    const int expected_errors[] = {ETIMEDOUT, EBADF, EIO};
    for (size_t index = 0; index < sizeof(poll_failures) / sizeof(poll_failures[0]); ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.send_actions[0] = (io_action_t){.result = -1, .error = EAGAIN};
        fault_state.send_action_count = 1;
        fault_state.poll_actions[0] = poll_failures[index];
        fault_state.poll_action_count = 1;
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1 && errno == expected_errors[index]);
        assert_response_unchanged(&response);
        assert(fcntl(pair[0], F_GETFD) >= 0);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }

    const clock_mode_t clock_failures[] = {ClockModeFailure, ClockModeNegative,
        ClockModeNegativeNanoseconds, ClockModeInvalidNanoseconds, ClockModeOverflow};
    const int clock_errors[] = {EIO, EOVERFLOW, EOVERFLOW, EOVERFLOW, EOVERFLOW};
    for (size_t index = 0; index < sizeof(clock_failures) / sizeof(clock_failures[0]); ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.clock_mode = clock_failures[index];
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1);
        assert(errno == clock_errors[index]);
        assert_response_unchanged(&response);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_deadline_and_clock_failures_inside_transfer(void) {
    const uint64_t before_first_io[] = {1000, 3321};
    const uint64_t after_eagain[] = {1000, 1000, 3321};
    for (size_t index = 0; index < 2; ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.clock_mode = ClockModeScripted;
        const uint64_t *values = index == 0 ? before_first_io : after_eagain;
        size_t value_count = index == 0 ? 2 : 3;
        memcpy(fault_state.clock_values, values, value_count * sizeof(values[0]));
        fault_state.clock_count = value_count;
        if (index == 1) {
            fault_state.send_actions[0] = (io_action_t){.result = -1, .error = EAGAIN};
            fault_state.send_action_count = 1;
        }
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1 && errno == ETIMEDOUT);
        assert_response_unchanged(&response);
        assert(fault_state.poll_action_index == 0);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }

    const size_t failure_indices[] = {1, 2, 2};
    for (size_t index = 0; index < sizeof(failure_indices) / sizeof(failure_indices[0]); ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.clock_fail_at_index = failure_indices[index];
        if (index == 1) {
            fault_state.send_actions[0] = (io_action_t){.result = -1, .error = EAGAIN};
            fault_state.send_action_count = 1;
        }
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1 && errno == EIO);
        assert_response_unchanged(&response);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_short_payload_read_and_send_terminal_errors(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyReady);
    fault_state.recv_actions[0] = (io_action_t){.result = WaddleDaemonHeaderSize};
    fault_state.recv_actions[1] = (io_action_t){.result = 3};
    fault_state.recv_actions[2] = (io_action_t){.result = 5};
    fault_state.recv_action_count = 3;
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    assert(run_client(pair[0], &response) == 0);
    assert(response.status == 0 && response.cid == 33);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);

    const io_action_t send_failures[] = {{.result = -1, .error = EPIPE}, {.result = 0}};
    const int expected_errors[] = {EPIPE, ECONNRESET};
    for (size_t index = 0; index < sizeof(send_failures) / sizeof(send_failures[0]); ++index) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.send_actions[0] = send_failures[index];
        fault_state.send_action_count = 1;
        response = (waddle_daemon_guest_ready_resp_t){.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1 && errno == expected_errors[index]);
        assert_response_unchanged(&response);
        assert(fcntl(pair[0], F_GETFD) >= 0);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_argument_guards_preserve_borrowed_socket(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    fault_state.enabled = 0;
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(waddle_client_guest_ready(-1, 321, &response) == -1 && errno == EINVAL);
    errno = 0;
    assert(waddle_client_guest_ready(pair[0], 321, NULL) == -1 && errno == EINVAL);
    errno = 0;
    assert(waddle_client_guest_ready(pair[0], 120001, &response) == -1 && errno == EINVAL);
    assert_response_unchanged(&response);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_receive_eof_and_system_error(void) {
    const io_action_t failures[] = {{.result = 0}, {.result = -1, .error = ECONNRESET}};
    const int errors[] = {ECONNRESET, ECONNRESET};
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(ReplyReady);
        fault_state.recv_actions[0] = (io_action_t){.result = WaddleDaemonHeaderSize};
        fault_state.recv_actions[1] = failures[index];
        fault_state.recv_action_count = 2;
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        assert(run_client(pair[0], &response) == -1 && errno == errors[index]);
        assert_response_unchanged(&response);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_invalid_headers_and_payloads_preserve_caller_response(void) {
    const reply_kind_t kinds[] = {ReplyBadMagic, ReplyBadVersion, ReplyOversizedHeader,
        ReplyWrongType, ReplyWrongSequence, ReplyWrongLength, ReplyErrorWrongLength, ReplyBadReadyPayload,
        ReplyBadErrorZero, ReplyBadErrorTooLarge};
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        reset_fault_state(kinds[index]);
        waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
        errno = 0;
        int result = run_client(pair[0], &response);
        assert(result == -1);
        assert(errno == EPROTO || errno == ENOTSUP);
        assert_response_unchanged(&response);
        assert(fcntl(pair[0], F_GETFD) >= 0);
        fault_state.enabled = 0;
        close(pair[0]);
        close(pair[1]);
    }
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyError);
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(run_client(pair[0], &response) == -1 && errno == ENOTSUP);
    assert_response_unchanged(&response);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);
}

static void test_last_payload_read_completing_at_deadline_is_rejected(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    reset_fault_state(ReplyReady);
    fault_state.clock_mode = ClockModeScripted;
    const uint64_t values[] = {1000, 1000, 1000, 1000, 1000, 3320, 3321};
    memcpy(fault_state.clock_values, values, sizeof(values));
    fault_state.clock_count = sizeof(values) / sizeof(values[0]);
    waddle_daemon_guest_ready_resp_t response = {.status = 91, .cid = 92};
    errno = 0;
    assert(run_client(pair[0], &response) == -1 && errno == ETIMEDOUT);
    assert_response_unchanged(&response);
    fault_state.enabled = 0;
    close(pair[0]);
    close(pair[1]);
}

int main(void) {
    test_valid_reply_and_sequence_from_actual_request();
    test_zero_timeout_selects_default_deadline();
    test_short_io_eintr_eagain_poll_eintr_and_readiness();
    test_eagain_poll_failures_and_clock_errors();
    test_deadline_and_clock_failures_inside_transfer();
    test_short_payload_read_and_send_terminal_errors();
    test_argument_guards_preserve_borrowed_socket();
    test_receive_eof_and_system_error();
    test_invalid_headers_and_payloads_preserve_caller_response();
    test_last_payload_read_completing_at_deadline_is_rejected();
    puts("test_guest_ready_client_faults: all tests passed");
    return 0;
}
