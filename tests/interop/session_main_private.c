#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define WaddleSessionNoMain
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static uint32_t session_private_poll_eintr;
static uint32_t session_private_poll_error;
static uint32_t session_private_poll_clear_events;
static uint32_t session_private_poll_eintr_after_random;
static uint32_t session_private_poll_delay_ms;
static uint32_t session_private_random_mode;
static uint32_t session_private_random_remaining;
static uint32_t session_private_random_partial;
static int session_private_signal_after_random = -1;
static uint32_t session_private_clock_error;
static uint32_t session_private_clock_delay;
static uint32_t session_private_clock_anomaly;
static uint32_t session_private_signal_fd_error;
static uint32_t session_private_short_socket_error;
static uint32_t session_private_socket_error;
static uint32_t session_private_get_sockopt_error;
static uint32_t session_private_socket_fail;
static uint32_t session_private_connect_pending;
static uint32_t session_private_connect_again;
static uint32_t session_private_connect_delay_ms;
static uint32_t session_private_mask_error;

static int session_private_poll(struct pollfd *descriptors, nfds_t count,
                               int timeout)
{
    if (session_private_poll_eintr != 0) {
        --session_private_poll_eintr;
        errno = EINTR;
        return -1;
    }
    if (session_private_poll_error != 0) {
        --session_private_poll_error;
        errno = EIO;
        return -1;
    }
    if (session_private_poll_eintr_after_random != 0 && timeout == 0) {
        --session_private_poll_eintr_after_random;
        errno = EINTR;
        return -1;
    }
    int result = poll(descriptors, count, timeout);
    if (result >= 0 && session_private_poll_delay_ms != 0) {
        uint32_t delay_ms = session_private_poll_delay_ms;
        session_private_poll_delay_ms = 0;
        struct timespec delay = {
            .tv_sec = (time_t)(delay_ms / 1000u),
            .tv_nsec = (long)(delay_ms % 1000u) * 1000000L,
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    }
    if (result > 0 && session_private_poll_clear_events != 0) {
        --session_private_poll_clear_events;
        for (nfds_t index = 0; index < count; ++index) descriptors[index].revents = 0;
    }
    return result;
}

static int session_private_getsockopt(int fd, int level, int option,
                                      void *value, socklen_t *length)
{
    int result = getsockopt(fd, level, option, value, length);
    if (result == 0 && session_private_short_socket_error != 0 &&
        level == SOL_SOCKET && option == SO_ERROR) {
        --session_private_short_socket_error;
        *length = sizeof(int) - 1;
    }
    if (result == 0 && session_private_socket_error != 0 &&
        level == SOL_SOCKET && option == SO_ERROR) {
        --session_private_socket_error;
        *(int *)value = ECONNREFUSED;
    }
    if (result == 0 && session_private_get_sockopt_error != 0 &&
        level == SOL_SOCKET && option == SO_ERROR) {
        --session_private_get_sockopt_error;
        errno = EIO;
        return -1;
    }
    return result;
}

static int session_private_socket(int domain, int type, int protocol)
{
    if (session_private_socket_fail != 0) {
        --session_private_socket_fail;
        errno = EMFILE;
        return -1;
    }
    return socket(domain, type, protocol);
}

static int session_private_connect(int fd, const struct sockaddr *address,
                                  socklen_t address_length)
{
    int result = connect(fd, address, address_length);
    if (result == 0 && session_private_connect_again != 0) {
        --session_private_connect_again;
        errno = EAGAIN;
        return -1;
    }
    if (result == 0 && session_private_connect_pending != 0) {
        --session_private_connect_pending;
        errno = EINPROGRESS;
        return -1;
    }
    if (result == 0 && session_private_connect_delay_ms != 0) {
        uint32_t delay_ms = session_private_connect_delay_ms;
        session_private_connect_delay_ms = 0;
        struct timespec delay = {
            .tv_sec = (time_t)(delay_ms / 1000u),
            .tv_nsec = (long)(delay_ms % 1000u) * 1000000L,
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    }
    return result;
}

static int session_private_pthread_sigmask(int how, const sigset_t *set,
                                           sigset_t *old_set)
{
    if (session_private_mask_error != 0) {
        --session_private_mask_error;
        return EPERM;
    }
    return pthread_sigmask(how, set, old_set);
}

static ssize_t session_private_getrandom(void *buffer, size_t length,
                                         unsigned int flags)
{
    if (session_private_random_mode != 0) {
        uint32_t mode = session_private_random_mode;
        if (mode == 3) return 0;
        if (mode == 4) {
            session_private_random_mode = 0;
            memset(buffer, 0, length);
            session_private_poll_eintr_after_random = 1;
            return (ssize_t)length;
        }
        if (mode == 6) {
            memset(buffer, 0, length);
            return (ssize_t)length;
        }
        if (mode == 7 || mode == 8 || mode == 9 || mode == 10) {
            session_private_random_mode = 0;
            ssize_t result = getrandom(buffer, length, flags);
            if (result > 0) {
                if (mode == 7) session_private_poll_eintr_after_random = 1;
                else if (mode == 8) session_private_poll_clear_events = 1;
                else if (mode == 9) session_private_clock_anomaly = 1;
                else session_private_clock_anomaly = 6;
            }
            return result;
        }
        if (mode == 5 && session_private_random_remaining != 0 &&
            --session_private_random_remaining == 0)
            session_private_random_mode = 0;
        errno = mode == 1 || mode == 5 ? EAGAIN : EINTR;
        return -1;
    }
    if (session_private_random_partial != 0) {
        --session_private_random_partial;
        size_t amount = length < 3 ? length : 3;
        memset(buffer, 0x5a, amount);
        return (ssize_t)amount;
    }
    ssize_t result = getrandom(buffer, length, flags);
    if (result > 0 && session_private_signal_after_random >= 0) {
        const uint8_t signal_byte = 1;
        assert(write(session_private_signal_after_random, &signal_byte, 1) == 1);
        session_private_signal_after_random = -1;
    }
    return result;
}

static int session_private_clock_gettime(clockid_t clock_id,
                                         struct timespec *time_value)
{
    if (session_private_clock_delay != 0) {
        --session_private_clock_delay;
        return clock_gettime(clock_id, time_value);
    }
    if (session_private_clock_error != 0) {
        --session_private_clock_error;
        errno = EIO;
        return -1;
    }
    int result = clock_gettime(clock_id, time_value);
    if (result == 0 && session_private_clock_anomaly != 0) {
        uint32_t anomaly = session_private_clock_anomaly;
        session_private_clock_anomaly = 0;
        if (anomaly == 1) time_value->tv_nsec = -1;
        else if (anomaly == 2) time_value->tv_nsec = 1000000000L;
        else if (anomaly == 3) time_value->tv_sec = -1;
        else if (anomaly == 4) time_value->tv_sec = (time_t)INT64_MAX;
        else if (anomaly == 5) {
            time_value->tv_sec = (time_t)(UINT64_MAX / UINT64_C(1000));
            time_value->tv_nsec = 0;
        } else {
            time_value->tv_sec = (time_t)(UINT64_MAX / UINT64_C(1000) - 10);
            time_value->tv_nsec = 0;
        }
    }
    return result;
}

static int session_private_signalfd(int fd, const sigset_t *mask, int flags)
{
    if (session_private_signal_fd_error != 0) {
        --session_private_signal_fd_error;
        errno = EIO;
        return -1;
    }
    return signalfd(fd, mask, flags);
}

#define poll session_private_poll
#define getrandom session_private_getrandom
#define getsockopt session_private_getsockopt
#define socket session_private_socket
#define connect session_private_connect
#define pthread_sigmask session_private_pthread_sigmask
#define clock_gettime session_private_clock_gettime
#define signalfd session_private_signalfd
#include "../../src/interop/session_main.c"
#undef signalfd
#undef clock_gettime
#undef getsockopt
#undef socket
#undef connect
#undef pthread_sigmask
#undef getrandom
#undef poll

void session_main_private_tests(void)
{
    uint32_t value = 77;
    assert(session_parse_decimal("1", &value) && value == 1);
    assert(session_parse_decimal("120000", &value) && value == 120000);
    assert(!session_parse_decimal(NULL, &value));
    assert(!session_parse_decimal("", &value));
    assert(!session_parse_decimal("0", &value));
    assert(!session_parse_decimal("120001", &value));
    assert(!session_parse_decimal("12x", &value));
    assert(!session_parse_decimal("-1", &value));
    assert(!session_parse_decimal("18446744073709551616", &value));
    assert(!session_parse_decimal("1", NULL));

    session_cli_options_t options;
    char *valid[] = {
        (char *)"waddle-session", (char *)"--control-only", (char *)"--device",
        (char *)"private-test", (char *)"--backing-kind", (char *)"kvmfr",
        (char *)"--run-for-ms", (char *)"120000",
    };
    assert(session_parse_args(8, valid, &options));
    assert(options.backing_kind == InteropMappingLinuxBackingKvmfrCharacter);
    assert(options.run_for_ms == 120000 && !options.help);
    char *duplicate_control[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--control-only", (char *)"--device", (char *)"private-test"};
    assert(!session_parse_args(5, duplicate_control, &options));
    char *duplicate_device[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"private-test", (char *)"--device",
        (char *)"private-test"};
    assert(!session_parse_args(6, duplicate_device, &options));
    char *control_without_device[] = {(char *)"waddle-session", (char *)"--control-only"};
    char *device_without_control[] = {(char *)"waddle-session", (char *)"--device",
        (char *)"private-test"};
    char *duplicate_backing[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"private-test", (char *)"--backing-kind",
        (char *)"file", (char *)"--backing-kind", (char *)"kvmfr"};
    char *bad_backing[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"private-test", (char *)"--backing-kind",
        (char *)"other"};
    char *duplicate_runtime[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"private-test", (char *)"--run-for-ms",
        (char *)"10", (char *)"--run-for-ms", (char *)"20"};
    char *bad_runtime[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"private-test", (char *)"--run-for-ms",
        (char *)"0"};
    char *help_overrides_invalid[] = {(char *)"waddle-session", (char *)"--help",
        (char *)"--control-only"};
    assert(!session_parse_args(2, control_without_device, &options));
    assert(!session_parse_args(3, device_without_control, &options));
    assert(!session_parse_args(8, duplicate_backing, &options));
    assert(!session_parse_args(6, bad_backing, &options));
    assert(!session_parse_args(8, duplicate_runtime, &options));
    assert(!session_parse_args(6, bad_runtime, &options));
    assert(session_parse_args(3, help_overrides_invalid, &options) && options.help);
    char *bad_name[] = {(char *)"waddle-session", (char *)"--control-only",
        (char *)"--device", (char *)"../invalid"};
    assert(!session_parse_args(4, bad_name, &options));
    char *help[] = {(char *)"waddle-session", (char *)"--help"};
    assert(session_parse_args(2, help, &options) && options.help);
    char *short_help[] = {(char *)"waddle-session", (char *)"-h"};
    assert(session_parse_args(2, short_help, &options) && options.help);
    char *valid_file_defaults[] = {(char *)"waddle-session",
        (char *)"--control-only", (char *)"--device", (char *)"private-test",
        (char *)"--backing-kind", (char *)"file", (char *)"--run-for-ms",
        (char *)"1"};
    assert(session_parse_args(8, valid_file_defaults, &options));
    assert(options.backing_kind == InteropMappingLinuxBackingRegularPrivateFile &&
        options.run_for_ms == 1);
    assert(!session_parse_args(0, valid, &options));
    assert(!session_parse_args(1, NULL, &options));
    assert(!session_parse_args(1, valid, NULL));
    char *missing_device_value[] = {(char *)"waddle-session",
        (char *)"--control-only", (char *)"--device"};
    char *missing_backing_value[] = {(char *)"waddle-session",
        (char *)"--control-only", (char *)"--device", (char *)"private-test",
        (char *)"--backing-kind"};
    char *missing_duration_value[] = {(char *)"waddle-session",
        (char *)"--control-only", (char *)"--device", (char *)"private-test",
        (char *)"--run-for-ms"};
    assert(!session_parse_args(3, missing_device_value, &options));
    assert(!session_parse_args(5, missing_backing_value, &options));
    assert(!session_parse_args(5, missing_duration_value, &options));

    assert(session_monotonic_ms() != UINT64_MAX);
    for (uint32_t anomaly = 1; anomaly <= 4; ++anomaly) {
        session_private_clock_anomaly = anomaly;
        assert(session_monotonic_ms() == UINT64_MAX);
    }
    session_private_clock_error = 1;
    assert(session_monotonic_ms() == UINT64_MAX && errno == EIO);
    session_private_clock_anomaly = 5;
    assert(session_monotonic_ms() > UINT64_MAX - UINT64_C(120000));

    int connected[2];
    int stop_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, connected) == 0);
    assert(pipe2(stop_pair, O_NONBLOCK | O_CLOEXEC) == 0);
    uint64_t deadline = session_monotonic_ms() + 1000;
    session_private_poll_eintr = 1;
    assert(session_wait_connect(connected[0], stop_pair[0], deadline) == 0);
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1500) == 0);
    session_private_poll_delay_ms = 5;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1) < 0 && errno == ETIMEDOUT);
    uint64_t delayed_clock_deadline = session_monotonic_ms() + 1000;
    session_private_clock_delay = 1;
    session_private_clock_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        delayed_clock_deadline) < 0 && errno == EIO);
    session_private_short_socket_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) < 0 && errno == EPROTO);
    session_private_socket_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) < 0 && errno == ECONNREFUSED);
    session_private_get_sockopt_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) < 0 && errno == EIO);
    session_private_poll_clear_events = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) == 0);
    session_private_poll_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) < 0 && errno == EIO);
    uint64_t clock_deadline = session_monotonic_ms() + 1000;
    session_private_clock_error = 1;
    assert(session_wait_connect(connected[0], stop_pair[0],
        clock_deadline) < 0 && errno == EIO);
    assert(session_wait_connect(connected[0], stop_pair[0], 1) != 0 &&
           errno == ETIMEDOUT);
    uint8_t fill_bytes[4096] = {0};
    for (;;) {
        ssize_t filled = send(connected[0], fill_bytes, sizeof(fill_bytes),
            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (filled < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        assert(filled > 0);
    }
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 5) < 0 && errno == ETIMEDOUT);
    const uint8_t stop_byte = 1;
    assert(write(stop_pair[1], &stop_byte, sizeof(stop_byte)) == sizeof(stop_byte));
    assert(session_wait_connect(connected[0], stop_pair[0],
        session_monotonic_ms() + 1000) != 0 && errno == ECANCELED);
    close(connected[0]);
    close(connected[1]);
    close(stop_pair[0]);
    close(stop_pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, connected) == 0);
    close(connected[1]);
    assert(session_wait_connect(connected[0], -1,
        session_monotonic_ms() + 1000) < 0 && errno == ECONNRESET);
    close(connected[0]);

    int random_pipe[2];
    assert(pipe2(random_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    uint64_t private_token = 0;
    session_private_poll_eintr = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) == 0 && private_token != 0);
    assert(session_read_random_token(&private_token, random_pipe[0], 1) != 0 &&
           errno == ETIMEDOUT);
    uint64_t random_deadline = session_monotonic_ms() + 1000;
    session_private_clock_error = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        random_deadline) != 0 && errno == EIO);
    session_private_poll_error = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EIO);
    session_private_random_mode = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EAGAIN);
    session_private_random_remaining = 64;
    session_private_random_mode = 5;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EAGAIN);
    session_private_random_mode = 0;
    session_private_random_mode = 6;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EIO);
    session_private_random_mode = 7;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) == 0 && private_token != 0);
    uint8_t random_stop_byte = 1;
    session_private_poll_clear_events = 1;
    assert(write(random_pipe[1], &random_stop_byte, sizeof(random_stop_byte)) ==
        sizeof(random_stop_byte));
    session_private_random_mode = 8;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) == 0 && private_token != 0);
    assert(read(random_pipe[0], &random_stop_byte, sizeof(random_stop_byte)) ==
        sizeof(random_stop_byte));
    session_private_random_mode = 9;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EIO);
    session_private_random_mode = 10;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == ETIMEDOUT);
    session_private_random_mode = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EAGAIN);
    session_private_random_mode = 2;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == EAGAIN);
    session_private_random_mode = 3;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) != 0);
    session_private_random_mode = 4;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) == 0 && private_token != 0);
    session_private_random_mode = 0;
    session_private_random_partial = 1;
    assert(session_read_random_token(&private_token, random_pipe[0],
        session_monotonic_ms() + 1000) == 0 && private_token != 0);
    close(random_pipe[0]);
    close(random_pipe[1]);

    uint64_t token = 0;
    int stop_pipe[2];
    assert(pipe2(stop_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    assert(session_read_random_token(&token, stop_pipe[0],
        session_monotonic_ms() + 1000) == 0);
    assert(token != 0);
    session_private_signal_after_random = stop_pipe[1];
    assert(session_read_random_token(&token, stop_pipe[0],
        session_monotonic_ms() + 1000) != 0 && errno == ECANCELED);
    assert(session_private_signal_after_random == -1);
    const uint8_t signal_byte = 1;
    assert(write(stop_pipe[1], &signal_byte, sizeof(signal_byte)) == sizeof(signal_byte));
    assert(session_read_random_token(&token, stop_pipe[0],
        session_monotonic_ms() + 1000) != 0);
    assert(errno == ECANCELED);
    uint8_t drained_signal = 0;
    assert(read(stop_pipe[0], &drained_signal, sizeof(drained_signal)) ==
        sizeof(drained_signal));
    assert(drained_signal == signal_byte);
    assert(session_connect_daemon(NULL, stop_pipe[0],
        session_monotonic_ms() + 1000) < 0 && errno == ENAMETOOLONG);
    assert(session_connect_daemon("", stop_pipe[0],
        session_monotonic_ms() + 1000) < 0 && errno == ENAMETOOLONG);
    char too_long_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
    memset(too_long_path, 'x', sizeof(too_long_path));
    too_long_path[sizeof(too_long_path) - 1] = '\0';
    assert(session_connect_daemon(too_long_path, stop_pipe[0],
        session_monotonic_ms() + 1000) < 0 && errno == ENAMETOOLONG);
    session_private_socket_fail = 1;
    assert(session_connect_daemon("/tmp/session-private.sock", stop_pipe[0],
        session_monotonic_ms() + 1000) < 0 && errno == EMFILE);
    char connect_path[] = "/tmp/session-main-connect-XXXXXX";
    int connect_temp = mkstemp(connect_path);
    assert(connect_temp >= 0 && close(connect_temp) == 0);
    assert(unlink(connect_path) == 0);
    int connect_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(connect_listener >= 0);
    struct sockaddr_un connect_address;
    memset(&connect_address, 0, sizeof(connect_address));
    connect_address.sun_family = AF_UNIX;
    assert(strlen(connect_path) < sizeof(connect_address.sun_path));
    strcpy(connect_address.sun_path, connect_path);
    assert(bind(connect_listener, (const struct sockaddr *)&connect_address,
        sizeof(connect_address)) == 0);
    assert(listen(connect_listener, 1) == 0);
    int connect_stop[2];
    assert(pipe2(connect_stop, O_NONBLOCK | O_CLOEXEC) == 0);
    session_private_connect_pending = 1;
    int connected_daemon = session_connect_daemon(connect_path, connect_stop[0],
        session_monotonic_ms() + 1000);
    assert(connected_daemon >= 0);
    int accepted_daemon = accept(connect_listener, NULL, NULL);
    assert(accepted_daemon >= 0);
    assert(close(connected_daemon) == 0);
    assert(close(accepted_daemon) == 0);
    session_private_connect_again = 1;
    connected_daemon = session_connect_daemon(connect_path, connect_stop[0],
        session_monotonic_ms() + 1000);
    assert(connected_daemon >= 0);
    accepted_daemon = accept(connect_listener, NULL, NULL);
    assert(accepted_daemon >= 0);
    assert(close(connected_daemon) == 0);
    assert(close(accepted_daemon) == 0);
    session_private_connect_delay_ms = 5;
    assert(session_connect_daemon(connect_path, connect_stop[0],
        session_monotonic_ms() + 1) < 0 && errno == ETIMEDOUT);
    accepted_daemon = accept(connect_listener, NULL, NULL);
    assert(accepted_daemon >= 0 && close(accepted_daemon) == 0);
    assert(close(connect_listener) == 0);
    assert(close(connect_stop[0]) == 0);
    assert(close(connect_stop[1]) == 0);
    assert(unlink(connect_path) == 0);
    assert(session_connect_daemon(connect_path, stop_pipe[0], 1) < 0);
    session_private_mask_error = 1;
    sigset_t prior_mask;
    assert(session_signals_begin(&prior_mask) < 0 && errno == EPERM);
    assert(session_connect_daemon("/tmp/waddle-session-missing.sock", stop_pipe[0], 1) < 0);
    close(stop_pipe[0]);
    close(stop_pipe[1]);
}

void session_main_private_fail_signalfd(void)
{
    session_private_signal_fd_error = 1;
}

void session_main_private_fail_clock(void)
{
    session_private_clock_error = 1;
}

void session_main_private_set_clock_anomaly(uint32_t anomaly)
{
    session_private_clock_anomaly = anomaly;
}

void session_main_private_set_random_mode(uint32_t mode)
{
    session_private_random_mode = mode;
}
