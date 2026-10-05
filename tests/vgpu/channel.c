/** @file channel.c @brief Real native stream framing, deadlines and ring backpressure. */
#include "waddle/venus_channel.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fault;
/* Native syscall fault boundary; normal cases below use real sockets and clocks. */
int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
    if (fault == 1)
        return -1;
    if (fault == 2 || fault == 3) {
        time->tv_sec = fault == 2 ? -1 : INT64_MAX;
        time->tv_nsec = 0;
        return 0;
    }
    return __real_clock_gettime(clock, time);
}
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (fault == 4 || fault == 5) {
        errno = fault == 4 ? EINTR : EIO;
        return -1;
    }
    if (fault == 6) {
        fds[0].revents = POLLNVAL;
        return 1;
    }
    if (fault >= 7 && fault <= 10) {
        fds[0].revents = fds[0].events;
        return 1;
    }
    return __real_poll(fds, count, timeout);
}
ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);
ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags) {
    if (fault >= 7 && fault <= 9) {
        errno = fault == 7 ? EINTR : (fault == 8 ? EAGAIN : EIO);
        return -1;
    }
    return __real_recv(fd, buffer, length, flags);
}
ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags) {
    if (fault == 10) {
        errno = EIO;
        return -1;
    }
    return __real_send(fd, buffer, length, flags);
}

static _Alignas(64) uint8_t mapping[4096];
static venus_session_t host;
static venus_session_t guest;
static venus_channel_t host_channel;
static venus_channel_t guest_channel;
static _Atomic uint32_t cancel;
static int sockets[2];

static void fresh(void) {
    fault = 0;
    atomic_store_explicit(&cancel, 0, memory_order_release);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&host, SessionHost, mapping, sizeof(mapping), 11) == RingOk);
    assert(venus_session_init(&guest, SessionGuest, mapping, sizeof(mapping), 0) == RingOk);
    assert(venus_channel_init(&host_channel, &host, sockets[0], &cancel) == RingOk);
    assert(venus_channel_init(&guest_channel, &guest, sockets[1], &cancel) == RingOk);
    assert(venus_channel_deadline(&host_channel, 1000) == RingOk);
    assert(venus_channel_deadline(&guest_channel, 1000) == RingOk);
}

static void cleanup(void) {
    fault = 0;
    venus_channel_free(&host_channel);
    venus_channel_free(&guest_channel);
    assert(!host_channel.initialized && !guest_channel.initialized);
    assert(fcntl(sockets[0], F_GETFL) >= 0); /* Channels never close borrowed streams. */
    assert(fcntl(sockets[1], F_GETFL) >= 0);
    close(sockets[0]);
    close(sockets[1]);
    venus_channel_free(&host_channel);
}

static void *host_handshake(void *argument) {
    (void)argument;
    assert(venus_channel_handshake(&host_channel) == RingOk);
    return NULL;
}
static void handshake(void) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, host_handshake, NULL) == 0);
    assert(venus_channel_handshake(&guest_channel) == RingOk);
    assert(pthread_join(thread, NULL) == 0);
    assert(venus_session_ready(&host) && venus_session_ready(&guest));
}
static void *drain_ring(void *argument) {
    (void)argument;
    const struct timespec Pause = {0, 5000000};
    nanosleep(&Pause, NULL);
    uint8_t bytes[64];
    assert(venus_ring_read(&host.region.commands, bytes, sizeof(bytes)) == RingOk);
    for (size_t index = 0; index < sizeof(bytes); index++)
        assert(bytes[index] == 0x5a);
    assert(venus_ring_read_wait(&host.region.commands, bytes, 32, venus_channel_wait,
                                &host_channel) == RingOk);
    for (size_t index = 0; index < 32; index++)
        assert(bytes[index] == 0x33);
    return NULL;
}
static void test_handshake_backpressure_and_stop(void) {
    fresh();
    handshake();
    assert(venus_channel_handshake(&host_channel) == RingInvalid);
    uint8_t bytes[64];
    memset(bytes, 0x5a, sizeof(bytes));
    assert(venus_ring_write(&guest.region.commands, bytes, sizeof(bytes)) == RingOk);
    memset(bytes, 0x33, sizeof(bytes));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, drain_ring, NULL) == 0);
    assert(venus_ring_write_wait(&guest.region.commands, bytes, 32, venus_channel_wait,
                                 &guest_channel) == RingOk);
    assert(pthread_join(thread, NULL) == 0);
    assert(venus_channel_stop(&guest_channel, StopCancel) == RingOk);
    assert(shutdown(sockets[1], SHUT_WR) == 0);
    assert(venus_channel_wait(&host_channel) == RingClosed);
    assert(host.reason == StopCancel);
    assert(venus_channel_wait(&host_channel) == RingClosed);
    cleanup();
}
static void ready_guest(void) {
    guest.state = SessionReady;
    guest.session_id = 11;
}
static void test_partial_frames_and_eof(void) {
    uint8_t frame[VenusControlBytes];
    const venus_control_t Stop = {.kind = ControlStop,
                                  .reason = StopDisconnect,
                                  .session_id = 11,
                                  .mapping_bytes = 4096,
                                  .capacity = 64};
    assert(venus_control_encode(&Stop, frame, sizeof(frame)) == RingOk);
    fresh();
    ready_guest();
    assert(venus_channel_wait(&guest_channel) == RingOk); /* No readable control. */
    for (size_t index = 0; index < sizeof(frame); index++) {
        assert(send(sockets[0], frame + index, 1, MSG_NOSIGNAL) == 1);
        assert(venus_channel_wait(&guest_channel) ==
               (index + 1 == sizeof(frame) ? RingClosed : RingOk));
        if (index == 0)
            assert(venus_channel_deadline(&guest_channel, 1000) == RingInvalid);
    }
    assert(guest.reason == StopDisconnect);
    cleanup();
    for (int partial = 0; partial < 2; partial++) {
        fresh();
        ready_guest();
        if (partial) {
            assert(send(sockets[0], frame, 7, MSG_NOSIGNAL) == 7);
            assert(venus_channel_wait(&guest_channel) == RingOk);
        }
        assert(shutdown(sockets[0], SHUT_WR) == 0);
        assert(venus_channel_wait(&guest_channel) == (partial ? RingCorrupt : RingClosed));
        assert(guest.reason == (partial ? StopProtocol : StopDisconnect));
        cleanup();
    }
    fresh();
    ready_guest();
    frame[0] ^= 1;
    assert(send(sockets[0], frame, sizeof(frame), MSG_NOSIGNAL) == sizeof(frame));
    assert(venus_channel_wait(&guest_channel) == RingCorrupt);
    cleanup();
}
static void *cancel_during_wait(void *context) {
    (void)context;
    const struct timespec Pause = {0, 3000000};
    nanosleep(&Pause, NULL);
    atomic_store_explicit(&cancel, 1, memory_order_release);
    return NULL;
}
static void test_deadlines_cancellation_and_flags(void) {
    fresh();
    assert(venus_channel_deadline(&guest_channel, 3) == RingOk);
    assert(venus_channel_handshake(&guest_channel) == RingTimeout);
    assert(guest.reason == StopDeadline);
    cleanup();
    fresh();
    atomic_store_explicit(&cancel, 1, memory_order_release);
    assert(venus_channel_handshake(&host_channel) == RingCancelled);
    assert(host.reason == StopCancel);
    cleanup();
    fresh();
    pthread_t cancelling;
    assert(pthread_create(&cancelling, NULL, cancel_during_wait, NULL) == 0);
    assert(venus_channel_handshake(&guest_channel) == RingCancelled);
    assert(pthread_join(cancelling, NULL) == 0);
    assert(guest.reason == StopCancel);
    cleanup();
    fresh();
    ready_guest();
    assert(venus_channel_deadline(&guest_channel, 1) == RingOk);
    const struct timespec Pause = {0, 3000000};
    nanosleep(&Pause, NULL);
    assert(venus_channel_wait(&guest_channel) == RingTimeout);
    cleanup();
    fresh();
    ready_guest();
    atomic_store_explicit(&cancel, 1, memory_order_release);
    assert(venus_channel_wait(&guest_channel) == RingCancelled);
    cleanup();
    for (int unknown = 0; unknown < 2; unknown++) {
        fresh();
        ready_guest();
        atomic_store_explicit(&guest.region.replies.header->flags, unknown ? 2 : VenusRingClosed,
                              memory_order_release);
        assert(venus_channel_wait(&guest_channel) == (unknown ? RingCorrupt : RingClosed));
        cleanup();
    }
}
static void test_local_and_native_errors(void) {
    venus_channel_t empty = {0};
    venus_channel_free(NULL);
    venus_channel_free(&empty);
    assert(venus_channel_init(NULL, &host, -1, NULL) == RingInvalid);
    assert(venus_channel_init(&empty, NULL, -1, NULL) == RingInvalid);
    fresh();
    assert(venus_channel_init(&empty, &host, -1, NULL) == RingInvalid);
    assert(venus_channel_init(&empty, &host, INTPTR_MAX, NULL) == RingInvalid);
    int blocking[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, blocking) == 0);
    assert(venus_channel_init(&empty, &host, blocking[0], NULL) == RingInvalid);
    close(blocking[0]);
    close(blocking[1]);
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, blocking) == 0);
    assert(venus_channel_init(&empty, &host, blocking[0], NULL) == RingInvalid);
    close(blocking[0]);
    close(blocking[1]);
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_NONBLOCK) == 0);
    assert(venus_channel_init(&empty, &host, pipe_fds[0], NULL) == RingInvalid);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    assert(venus_channel_init(&empty, &host, blocking[0], NULL) == RingInvalid); /* Closed fd. */
    assert(venus_channel_deadline(NULL, 1) == RingInvalid);
    assert(venus_channel_deadline(&empty, 1) == RingInvalid);
    assert(venus_channel_deadline(&host_channel, 0) == RingInvalid);
    assert(venus_channel_deadline(&host_channel, 60001) == RingInvalid);
    assert(venus_channel_handshake(NULL) == RingInvalid);
    assert(venus_channel_handshake(&empty) == RingInvalid);
    host_channel.deadline_ms = 0;
    assert(venus_channel_handshake(&host_channel) == RingInvalid);
    assert(venus_channel_wait(NULL) == RingInvalid);
    assert(venus_channel_wait(&empty) == RingInvalid);
    assert(venus_channel_wait(&host_channel) == RingInvalid);
    assert(venus_channel_stop(NULL, StopCancel) == RingInvalid);
    assert(venus_channel_stop(&empty, StopCancel) == RingInvalid);
    assert(venus_channel_stop(&host_channel, StopCancel) == RingInvalid);
    assert(venus_channel_deadline(&host_channel, 1000) == RingOk);
    assert(venus_channel_wait(&host_channel) == RingInvalid);
    assert(venus_channel_stop(&host_channel, StopNone) == RingInvalid);
    cleanup();
    for (int stage = 1; stage <= 10; stage++) {
        fresh();
        ready_guest();
        fault = stage;
        if (stage <= 3) {
            assert(venus_channel_deadline(&guest_channel, 1000) == RingClosed);
            assert(venus_channel_wait(&guest_channel) == RingClosed);
        } else if (stage == 4 || stage == 7 || stage == 8)
            assert(venus_channel_wait(&guest_channel) == RingOk);
        else if (stage == 10)
            assert(venus_channel_stop(&guest_channel, StopCancel) == RingClosed);
        else
            assert(venus_channel_wait(&guest_channel) == RingClosed);
        cleanup();
    }
    fresh();
    ready_guest();
    fault = 1;
    assert(venus_channel_wait(&guest_channel) == RingClosed);
    cleanup();
    fresh();
    assert(venus_channel_init(&empty, &host, sockets[0], NULL) == RingOk);
    assert(venus_channel_deadline(&empty, 1000) == RingOk);
    assert(shutdown(sockets[1], SHUT_RD) == 0);
    assert(venus_channel_handshake(&empty) == RingClosed);
    venus_channel_free(&empty);
    cleanup();
    fresh();
    assert(venus_channel_deadline(&host_channel, 2) == RingOk);
    assert(venus_channel_handshake(&host_channel) == RingTimeout); /* Offer delivered, no Ack. */
    cleanup();
}
int main(void) {
    test_handshake_backpressure_and_stop();
    test_partial_frames_and_eof();
    test_deadlines_cancellation_and_flags();
    test_local_and_native_errors();
    puts("Native Venus framing, handoff, backpressure, cancellation and EOF passed");
    return 0;
}
