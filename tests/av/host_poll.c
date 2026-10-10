/** @file host_poll.c @brief Actual production poll boundary under EINTR and expiry. */
#include "av_lease.h"
#include "av_wayland.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
static av_lease_host_t fixture_lease;
static uint64_t fixture_now;
static int poll_result, poll_errno, poll_calls, elapsed, flush_error, writable;
static int fixture_timeout(av_wayland_t *client, int maximum) {
    (void)client;
    errno = ERANGE; /* Deadline reads must not destroy poll's EINTR. */
    if (av_lease_host_check(&fixture_lease, fixture_now) != 0) return -1;
    return av_lease_timeout(fixture_lease.deadline, fixture_now, maximum);
}
static int fixture_flush(av_wayland_t *client) {
    (void)client;
    writable = 1; /* EAGAIN retains buffered sync and requests writable polling. */
    return flush_error;
}
static int fixture_writable(const av_wayland_t *client) { (void)client; return writable; }
static int fixture_fd(av_wayland_t *client) { (void)client; return 8; }
static int fixture_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 2 && fds[0].fd == 7 && fds[1].fd == 8);
    assert(fds[1].events & POLLOUT);
    uint64_t remaining = fixture_lease.deadline - fixture_now;
    assert(timeout == (remaining < 1000 ? (int)remaining : 1000));
    ++poll_calls;
    fixture_now += (uint64_t)elapsed;
    errno = poll_errno;
    return poll_result;
}
#define av_wayland_timeout fixture_timeout
#define av_wayland_flush fixture_flush
#define av_wayland_writable fixture_writable
#define av_wayland_fd fixture_fd
#define poll fixture_poll
#define main av_host_poll_fixture_unused_main
#include "../../src/av/av_host.c"
#undef main
static void reset(void) {
    fixture_lease = (av_lease_host_t){.epoch = 1, .deadline = 2100, .phase = AvLeaseBarrier};
    fixture_now = 100; elapsed = 600; poll_result = -1; poll_errno = EINTR;
    poll_calls = flush_error = writable = 0;
}
int main(void) {
    host_av_t host = {0}; struct pollfd fds[2];
    reset();
    assert(host_poll(&host, 7, fds) == -1 && errno == EINTR && fixture_lease.deadline == 2100);
    assert(host_poll(&host, 7, fds) == -1 && errno == EINTR && fixture_lease.deadline == 2100);
    assert(host_poll(&host, 7, fds) == -1 && errno == EINTR && fixture_lease.deadline == 2100);
    elapsed = 200;
    assert(host_poll(&host, 7, fds) == -2 && fixture_lease.phase == AvLeaseHostTerminal);
    assert(poll_calls == 4);
    assert(host_poll(&host, 7, fds) == -2 && poll_calls == 4);
    av_message_t late = {.type = MsgInputEpochReady, .lease_generation = 1, .buffer_index = 1};
    assert(av_lease_host_receive(&fixture_lease, &late, fixture_now) == -1);
    reset(); poll_result = 1; elapsed = 2000;
    assert(host_poll(&host, 7, fds) == -2); /* Readable late traffic cannot rescue expiry. */
    reset(); poll_result = 0; elapsed = 1000;
    assert(host_poll(&host, 7, fds) == 0 && fixture_lease.deadline == 2100);
    reset(); poll_errno = EIO;
    assert(host_poll(&host, 7, fds) == -1 && errno == EIO);
    reset(); flush_error = -1;
    assert(host_poll(&host, 7, fds) == -2 && !poll_calls);
    reset(); fixture_now = 0;
    assert(host_poll(&host, 7, fds) == -2 && !poll_calls);
    puts("AV host poll: EINTR, errno preservation, EAGAIN interest and unchanged deadline expiry passed");
    return 0;
}
