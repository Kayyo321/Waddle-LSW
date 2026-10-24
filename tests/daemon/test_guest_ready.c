/** @file test_guest_ready.c @brief Deterministic syscall faults for the real probe owner. */
#include "daemon_guest_ready.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>

static int socket_failure;
static int connect_error;
static int completion_error;
static int getsockopt_failure;
static int short_completion;
static int sockets;
static int closes;
static int last_fd;

int __wrap_socket(int domain, int type, int protocol) {
    assert(domain == AF_VSOCK && type == (SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC));
    assert(protocol == 0);
    sockets++;
    if (socket_failure) { errno = EMFILE; return -1; }
    last_fd = 100 + sockets;
    return last_fd;
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t bytes) {
    assert(fd == last_fd && bytes == sizeof(struct sockaddr_vm));
    const struct sockaddr_vm *vm_address = (const struct sockaddr_vm *)address;
    assert(vm_address->svm_family == AF_VSOCK && vm_address->svm_cid == 33);
    assert(vm_address->svm_port == 5249);
    if (connect_error) { errno = connect_error; return -1; }
    return 0;
}

int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *bytes) {
    assert(fd == last_fd && level == SOL_SOCKET && option == SO_ERROR);
    assert(*bytes == sizeof(int));
    if (getsockopt_failure) { errno = EIO; return -1; }
    *(int *)value = completion_error;
    if (short_completion) *bytes = 1;
    return 0;
}

int __wrap_close(int fd) {
    assert(fd == last_fd);
    closes++;
    errno = EINTR; /* Ownership must still be relinquished, without retry. */
    return -1;
}

static void reset(void) {
    socket_failure = connect_error = completion_error = getsockopt_failure = short_completion = 0;
    sockets = closes = 0;
    last_fd = -1;
}

static void test_arguments_and_completion(void) {
    reset();
    daemon_guest_ready_t probe;
    daemon_guest_ready_init(&probe);
    daemon_guest_ready_t original = probe;
    assert(daemon_guest_ready_start(NULL, 33, 5249, 1, 1, 1) == -1 && errno == EINVAL);
    const uint32_t invalid_cids[] = {0, 1, 2, UINT32_MAX};
    for (size_t index = 0; index < sizeof(invalid_cids) / sizeof(invalid_cids[0]); index++)
        assert(daemon_guest_ready_start(&probe, invalid_cids[index], 5249, 1, 1, 1) == -1);
    assert(daemon_guest_ready_start(&probe, 33, 0, 1, 1, 1) == -1);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 0, 1, 1) == -1);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 120001, 1, 1) == -1);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1, UINT64_MAX, 1) == -1);
    assert(memcmp(&probe, &original, sizeof(probe)) == 0 && sockets == 0);
    waddle_daemon_guest_ready_resp_t response = {.status = 99, .cid = 88};
    assert(daemon_guest_ready_step(&probe, 1, 1, 0, &response) == 0);
    assert(response.status == 99 && response.cid == 88);
    probe.probe_fd = 9;
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1, 1, 1) == -1 && errno == EBUSY);
    probe.probe_fd = -1;
    assert(daemon_guest_ready_start(&probe, 33, 5249, 120000, 100, 77) == 0);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1, 100, 77) == -1 && errno == EBUSY);
    assert(daemon_guest_ready_step(&probe, 1, 100, 0, &response) == 1);
    assert(response.status == 0 && response.cid == 33 && probe.sequence == 77);
    assert(!probe.active && probe.probe_fd == -1 && sockets == 1 && closes == 1);
    daemon_guest_ready_cancel(&probe);
    assert(closes == 1);
}

static void test_retry_deadline_and_disconnect(void) {
    reset();
    daemon_guest_ready_t probe;
    daemon_guest_ready_init(&probe);
    waddle_daemon_guest_ready_resp_t response = {.status = 99, .cid = 88};
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1000, 0, 1) == 0);
    socket_failure = 1;
    assert(daemon_guest_ready_step(&probe, 1, 0, 0, &response) == 0);
    assert(probe.retry_at_ms == 250 && closes == 0);
    assert(daemon_guest_ready_step(&probe, 1, 249, 0, &response) == 0 && sockets == 1);
    socket_failure = 0;
    connect_error = ECONNREFUSED;
    assert(daemon_guest_ready_step(&probe, 1, 250, 0, &response) == 0 && closes == 1);
    connect_error = EINPROGRESS;
    assert(daemon_guest_ready_step(&probe, 1, 500, 0, &response) == 0);
    assert(probe.probe_deadline_ms == 1000 && probe.probe_fd == last_fd);
    assert(daemon_guest_ready_step(&probe, 1, 501, 0, &response) == 0);
    assert(response.status == 99);
    assert(daemon_guest_ready_step(&probe, 1, 1000, POLLOUT, &response) == 1);
    assert(response.status == -ETIMEDOUT && closes == 2 && !probe.active);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1000, 1000, 2) == 0);
    assert(daemon_guest_ready_step(&probe, 0, 1000, 0, &response) == 1);
    assert(response.status == -ENOTCONN && sockets == 3);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 1000, 1000, 3) == 0);
    assert(daemon_guest_ready_step(&probe, 1, 1000, 0, &response) == 0);
    assert(daemon_guest_ready_step(&probe, 0, 1001, POLLOUT, &response) == 1);
    assert(response.status == -ENOTCONN && closes == 3);
}

static void test_completion_faults_and_cancel(void) {
    const short events[] = {POLLERR, POLLHUP, POLLNVAL, POLLOUT, POLLOUT, POLLOUT, 0, POLLOUT};
    for (size_t index = 0; index < sizeof(events) / sizeof(events[0]); index++) {
        reset();
        daemon_guest_ready_t probe;
        daemon_guest_ready_init(&probe);
        waddle_daemon_guest_ready_resp_t response = {.status = 99, .cid = 88};
        connect_error = index == 1 ? EALREADY : index == 2 ? EINTR : EINPROGRESS;
        getsockopt_failure = index == 3;
        short_completion = index == 4;
        completion_error = index == 5 ? ECONNREFUSED : 0;
        assert(daemon_guest_ready_start(&probe, 33, 5249, 10000, 0, 1) == 0);
        assert(daemon_guest_ready_step(&probe, 1, 0, 0, &response) == 0);
        assert(probe.probe_deadline_ms == 2000);
        int done = daemon_guest_ready_step(&probe, 1, index == 6 ? 2000 : 100, events[index], &response);
        assert(done == (index == 7));
        assert(closes == 1);
        assert(response.status == (index == 7 ? 0 : 99));
        if (!done) assert(probe.retry_at_ms == (index == 6 ? 2250 : 350));
        daemon_guest_ready_cancel(&probe);
        daemon_guest_ready_cancel(&probe);
        assert(closes == 1 && !probe.active);
    }
    reset();
    daemon_guest_ready_t probe;
    daemon_guest_ready_init(&probe);
    connect_error = EINPROGRESS;
    waddle_daemon_guest_ready_resp_t response;
    assert(daemon_guest_ready_start(&probe, 33, 5249, 200, 0, 1) == 0);
    assert(daemon_guest_ready_step(&probe, 1, 0, 0, &response) == 0);
    daemon_guest_ready_cancel(&probe);
    assert(closes == 1 && !probe.active && probe.probe_fd == -1);
    assert(daemon_guest_ready_start(&probe, 33, 5249, 100, 0, 1) == 0);
    connect_error = ECONNREFUSED;
    assert(daemon_guest_ready_step(&probe, 1, 0, 0, &response) == 0);
    assert(probe.retry_at_ms == 100);
    daemon_guest_ready_cancel(&probe);
}

int main(void) {
    test_arguments_and_completion();
    test_retry_deadline_and_disconnect();
    test_completion_faults_and_cancel();
    puts("guest readiness state machine: all deterministic faults passed");
    return 0;
}
