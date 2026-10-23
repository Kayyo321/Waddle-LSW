/**
 * @file session_shutdown.c
 * @brief Deterministic anonymous-socket regressions for CLI terminal delivery races.
 */

#include "session.h"
#include "terminal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int watched_fd = -1;
static int fragment_ack_fd = -1;
static int output_ack_fd = -1;
static unsigned int failed_writes = 0;
static int quiet = 0;

/* Linker wrappers observe real syscalls. They never replace their results. The
 * fragment acknowledgement proves the decoder hit EAGAIN before its tail arrives. */
ssize_t __real_read(int fd, void *buffer, size_t count);
ssize_t __real_write(int fd, const void *buffer, size_t count);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);

/**
 * @brief Forward a real read; acknowledge the first incomplete transport read.
 * @param[in] fd Borrowed descriptor; never closed by this wrapper.
 * @param[out] buffer Borrowed writable buffer of count bytes; NULL allowed only for zero count.
 * @param[in] count Maximum byte count.
 * @return Original read result and errno unchanged.
 * @note Test-process-local observation state; not thread-safe. No allocation.
 */
ssize_t __wrap_read(int fd, void *buffer, size_t count) {
    ssize_t result = __real_read(fd, buffer, count);
    int error = errno;
    if (fd == watched_fd && result < 0 && error == EAGAIN && fragment_ack_fd >= 0) {
        assert(__real_write(fragment_ack_fd, "R", 1) == 1);
        close(fragment_ack_fd);
        fragment_ack_fd = -1;
    }
    errno = error;
    return result;
}

/**
 * @brief Forward a real write; count terminal transport failures for assertions.
 * @param[in] fd Borrowed descriptor; never closed by this wrapper.
 * @param[in] buffer Borrowed readable buffer of count bytes; NULL allowed only for zero count.
 * @param[in] count Maximum byte count.
 * @return Original write result and errno unchanged.
 * @note Test-process-local observation state; not thread-safe. No allocation.
 */
ssize_t __wrap_write(int fd, const void *buffer, size_t count) {
    ssize_t result = __real_write(fd, buffer, count);
    if (fd == watched_fd && result < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        failed_writes++;
    }
    return result;
}

/**
 * @brief Observe the real session waiting to deliver output after validated exit.
 * @param[in,out] fds Borrowed array of count descriptors; passed through unchanged.
 * @param[in] count Array element count.
 * @param[in] timeout Original millisecond timeout, including -1 for unlimited.
 * @return Original poll result and errno unchanged.
 * @note Test-process-local state; not thread-safe. Never changes readiness or timing.
 */
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (watched_fd >= 0 && output_ack_fd >= 0 && count == 5 &&
        fds[0].fd == -1 && fds[2].fd == STDOUT_FILENO && (fds[2].events & POLLOUT)) {
        assert(__real_write(output_ack_fd, "O", 1) == 1);
        close(output_ack_fd);
        output_ack_fd = -1;
    }
    return __real_poll(fds, count, timeout);
}

typedef enum shutdown_case_t {
    ClosedSuccess,
    ClosedNonzero,
    NoProcessExit,
    TruncatedExit,
    TruncatedHeader,
    EarlyExit,
    CorruptExit,
    FragmentedExit,
    HeldOpen,
    HeldOpenWithSignals,
    EarlierDeadline,
    InputEofFirst,
    OutputFailure,
    BlockedOutputDeadline,
    BlockedOutputDelivery,
    ShutdownCaseCount
} shutdown_case_t;

static void full_write(int fd, const void *buffer, size_t length) {
    const uint8_t *bytes = buffer;
    while (length > 0) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        bytes += count;
        length -= (size_t)count;
    }
}

static void run_case(shutdown_case_t test_case) {
    int sockets[2], output[2], errors[2], fragment_ack[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(output, O_CLOEXEC) == 0 && pipe2(errors, O_CLOEXEC) == 0);
    assert(pipe2(fragment_ack, O_CLOEXEC) == 0);
    assert(nonblock(sockets[0]) == 0);
    queue_t reply;
    assert(queue_init(&reply) == 0);
    uint32_t sequence = 1;
    uint8_t response[12] = {0};
    waddle_put32(response + 4, 123);
    assert(wire_send(&reply, &sequence, WaddleMsgSpawnResp, response, sizeof(response)) == 0);
    size_t spawn_length = reply.len;
    assert(wire_stream(&reply, &sequence, WaddleStreamStdout, "final stdout\n", 13) == 0);
    assert(wire_stream(&reply, &sequence, WaddleStreamStderr, "final stderr\n", 13) == 0);
    if (test_case != EarlyExit) {
        assert(wire_eof(&reply, &sequence, WaddleStreamStdout) == 0);
        assert(wire_eof(&reply, &sequence, WaddleStreamStderr) == 0);
    }
    size_t exit_offset = reply.len;
    uint8_t exit_body[16] = {0};
    waddle_put32(exit_body, test_case == ClosedNonzero ? 42 : 0);
    assert(wire_send(&reply, &sequence, WaddleMsgProcessExit, exit_body, sizeof(exit_body)) == 0);
    if (test_case == CorruptExit) reply.data[exit_offset + 28] ^= 1;

    size_t first_length = reply.len;
    if (test_case == TruncatedExit || test_case == FragmentedExit) first_length -= 8;
    if (test_case == TruncatedHeader) first_length = exit_offset + 17;
    if (test_case == NoProcessExit || test_case == HeldOpen ||
        test_case == HeldOpenWithSignals || test_case == EarlierDeadline) first_length = exit_offset;
    if (test_case == InputEofFirst) first_length = spawn_length;
    full_write(sockets[1], reply.data, first_length);

    int keep_open = test_case == FragmentedExit || test_case == HeldOpen ||
                    test_case == HeldOpenWithSignals || test_case == EarlierDeadline ||
                    test_case == InputEofFirst;
    if (test_case != InputEofFirst) assert(shutdown(sockets[1], SHUT_RD) == 0);
    if (!keep_open) {
        close(sockets[1]);
        sockets[1] = -1;
    }
    if (test_case == OutputFailure) {
        close(output[0]);
        output[0] = -1;
    }

    size_t output_prefill = 0;
    int blocked_output = test_case == BlockedOutputDeadline || test_case == BlockedOutputDelivery;
    if (blocked_output) {
        assert(nonblock(output[1]) == 0);
        char padding[4096];
        memset(padding, 'P', sizeof(padding));
        for (;;) {
            ssize_t written = write(output[1], padding, sizeof(padding));
            if (written < 0) { assert(errno == EAGAIN); break; }
            assert(written > 0);
            output_prefill += (size_t)written;
        }
        assert(output_prefill > 0);
    }

    uint64_t started = monotonic_ms();
    pid_t session = fork();
    assert(session >= 0);
    if (session == 0) {
        if (sockets[1] >= 0) close(sockets[1]);
        if (output[0] >= 0) close(output[0]);
        close(errors[0]);
        close(fragment_ack[0]);
        assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
        assert(dup2(errors[1], STDERR_FILENO) == STDERR_FILENO);
        close(output[1]);
        close(errors[1]);
        int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(input >= 0 && dup2(input, STDIN_FILENO) == STDIN_FILENO);
        close(input);
        queue_free(&reply);
        queue_t tx;
        assert(queue_init(&tx) == 0);
        assert(waddle_terminal_init(0) == 0);
        watched_fd = sockets[0];
        if (test_case == FragmentedExit) fragment_ack_fd = fragment_ack[1];
        else if (blocked_output) output_ack_fd = fragment_ack[1];
        else close(fragment_ack[1]);
        uint64_t deadline = test_case == EarlierDeadline ? monotonic_ms() + 100 :
                            (test_case == BlockedOutputDeadline ? monotonic_ms() + 500 : 0);
        int result = waddle_session(sockets[0], &tx, 1, deadline, 0);
        if (test_case != InputEofFirst && test_case != OutputFailure) assert(failed_writes == 1);
        if (test_case == InputEofFirst) assert(failed_writes == 0);
        if (test_case != OutputFailure) assert(tx.len == 0);
        queue_free(&tx);
        waddle_terminal_close();
        close(sockets[0]);
        if (fragment_ack_fd >= 0) close(fragment_ack_fd);
        if (output_ack_fd >= 0) close(output_ack_fd);
        exit(result);
    }
    close(sockets[0]);
    close(output[1]);
    close(errors[1]);
    close(fragment_ack[1]);
    assert(nonblock(errors[0]) == 0);
    if (output[0] >= 0) assert(nonblock(output[0]) == 0);

    char captured[2][512] = {{0}, {0}};
    size_t used[2] = {0, 0};
    int status = 0;
    int finished = 0;
    int fragment_completed = 0;
    int input_eof_received = 0;
    int output_ack_received = 0;
    int output_released = !blocked_output;
    unsigned int signals_sent = 0;
    while (!finished || output[0] >= 0 || errors[0] >= 0) {
        if (monotonic_ms() >= started + 6000) {
            kill(session, SIGKILL);
            assert(!"session shutdown regression exceeded its outer six-second bound");
        }
        struct pollfd ready[4] = {
            {output_released || finished ? output[0] : -1, POLLIN, 0}, {errors[0], POLLIN, 0},
            {(test_case == FragmentedExit && !fragment_completed) ||
             (blocked_output && !output_ack_received) ? fragment_ack[0] : -1, POLLIN, 0},
            {test_case == InputEofFirst && !input_eof_received ? sockets[1] : -1, POLLIN, 0}
        };
        int polled = poll(ready, 4, 20);
        if (polled < 0 && errno == EINTR) continue;
        assert(polled >= 0);
        for (int i = 0; i < 2; i++) {
            if (ready[i].revents == 0) continue;
            int *fd = i == 0 ? &output[0] : &errors[0];
            uint8_t bytes[4096];
            ssize_t got = read(*fd, bytes, sizeof(bytes));
            if (got > 0) {
                size_t offset = 0;
                while (i == 0 && output_prefill > 0 && offset < (size_t)got) {
                    assert(bytes[offset++] == 'P');
                    output_prefill--;
                }
                size_t payload = (size_t)got - offset;
                assert(payload < sizeof(captured[i]) - used[i]);
                memcpy(captured[i] + used[i], bytes + offset, payload);
                used[i] += payload;
            }
            else if (got == 0) { close(*fd); *fd = -1; }
            else assert(errno == EAGAIN || errno == EINTR);
        }
        if (ready[2].revents != 0) {
            char acknowledged;
            assert(read(fragment_ack[0], &acknowledged, 1) == 1);
            if (blocked_output) {
                assert(acknowledged == 'O');
                if (test_case == BlockedOutputDelivery) {
                    assert(waitpid(session, &status, WNOHANG) == 0);
                }
                output_ack_received = 1;
                output_released = test_case == BlockedOutputDelivery;
            } else {
                assert(acknowledged == 'R');
                full_write(sockets[1], reply.data + first_length, reply.len - first_length);
                close(sockets[1]);
                sockets[1] = -1;
                fragment_completed = 1;
            }
        }
        if (ready[3].revents != 0) {
            decoder_t request = {0};
            assert(wire_read(&request, sockets[1]) == 1);
            assert(request.type == WaddleMsgStreamEof && request.length == 4 &&
                   waddle_get32(request.body) == WaddleStreamStdin);
            wire_destroy(&request);
            full_write(sockets[1], reply.data + first_length, reply.len - first_length);
            close(sockets[1]);
            sockets[1] = -1;
            input_eof_received = 1;
        }
        if (!finished) {
            pid_t waited = waitpid(session, &status, WNOHANG);
            assert(waited == 0 || waited == session);
            finished = waited == session;
        }
        if (!finished && test_case == HeldOpenWithSignals && used[0] > 0) {
            assert(kill(session, SIGTERM) == 0);
            signals_sent++;
        }
    }
    uint64_t elapsed = monotonic_ms() - started;
    if (sockets[1] >= 0) close(sockets[1]);
    close(fragment_ack[0]);
    queue_free(&reply);
    int valid = test_case == ClosedSuccess || test_case == ClosedNonzero ||
                test_case == FragmentedExit || test_case == InputEofFirst || test_case == BlockedOutputDelivery;
    int expected = valid ? (test_case == ClosedNonzero ? 42 : 0) :
                   ((test_case == EarlierDeadline || test_case == BlockedOutputDeadline) ? 124 : 125);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
        fprintf(stderr, "shutdown case %d: status %#x, expected %d; stderr: %s\n",
                test_case, status, expected, captured[1]);
        abort();
    }
    if (test_case != OutputFailure) {
        assert(strcmp(captured[0], test_case == BlockedOutputDeadline ? "" : "final stdout\n") == 0);
    }
    assert(output_prefill == 0);
    if (valid) assert(strcmp(captured[1], "final stderr\n") == 0);
    else assert(strstr(captured[1], (test_case == EarlierDeadline || test_case == BlockedOutputDeadline) ? "session timed out" : "session failed") != NULL);
    if (test_case == FragmentedExit) assert(fragment_completed);
    if (test_case == InputEofFirst) assert(input_eof_received);
    if (test_case == HeldOpen || test_case == HeldOpenWithSignals) assert(elapsed >= 1900 && elapsed < 6000);
    if (test_case == HeldOpenWithSignals) assert(signals_sent > 1);
    if (test_case == EarlierDeadline || test_case == BlockedOutputDeadline) assert(elapsed < 1900);
    if (blocked_output) assert(output_ack_received);
    if (!quiet) printf("session shutdown: case %d passed (exit %d)\n", test_case, expected);
}

int main(int argc, char **argv) {
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--stress") == 0) {
        quiet = 1;
        for (unsigned int iteration = 0; iteration < 128; iteration++) {
            run_case(ClosedSuccess);
            run_case(ClosedNonzero);
            run_case(FragmentedExit);
            run_case(InputEofFirst);
        }
        puts("session shutdown: 512 bounded production-session stress cases passed");
        return 0;
    }
    assert(argc == 1);
    for (shutdown_case_t test_case = ClosedSuccess; test_case < ShutdownCaseCount; test_case++) {
        run_case(test_case);
    }
    printf("session shutdown: all %d cases passed\n", ShutdownCaseCount);
    return 0;
}
