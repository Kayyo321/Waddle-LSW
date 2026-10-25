/**
 * @file linux_socket_owner.c
 * @brief Real socketpair and fault-wrapper tests for the Linux socket owner.
 * @ownership Test owns every socket, owner and caller buffer; all are released
 * before exit, including failure-path owners.
 * @thread_safety Single-threaded, matching the production owner contract.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waddle/interop_linux_socket.h"
#include "waddle/interop_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(sizeof(interop_linux_socket_config_t) == 56, "config ABI");
_Static_assert(sizeof(interop_linux_socket_poll_t) == 32, "poll ABI");
_Static_assert(sizeof(interop_linux_socket_work_t) == 152, "work ABI");

static int fail_next_ack;
static int fail_peer_call;
static unsigned fail_transport_lost_count;
static int fail_feed_after_recv;
static int report_oversized_feed_consumed;
static int report_zero_feed_consumed;
static int fail_pending_after_recv;
static int forced_peer_create_result = -1;
static int forced_mapping_result = -1;
static int forced_drain_result = -1;
static int fail_next_mapping;
static int fail_next_drain;
static int fail_next_close;
static int fail_next_so_error;
static int fail_next_so_error_errno;
static int injected_so_error;
static int fail_next_recv_errno;
static int return_zero_recv;
static int fail_next_send_errno;
static int return_zero_send;
static int suppress_pending_after_recv;
static int arm_pending_suppression_after_recv;
static size_t recv_chunk_limit;
static unsigned recv_chunk_two_byte_calls;
static unsigned recv_chunk_schedule_calls;
static unsigned wrapped_close_calls;
static unsigned total_close_calls;
static int fail_next_calloc;
static int fail_next_getfl;
static int fail_next_getfd;
static int override_copy_tx_length;
static unsigned peer_next_event_calls;
static int force_abort_next_event;
static int override_get_state_phase = -1;
static int override_tick_phase = -1;
static int override_mapping_phase = -1;
static int override_drain_phase = -1;
static int override_feed_phase = -1;
static int override_next_event_phase = -1;
static int override_ack_event_phase = -1;

extern void interop_linux_socket_test_guards(void);
extern void test_linux_socket_owner_private(void);

enum {
    PeerCallNone = 0,
    PeerCallGetState,
    PeerCallFeed,
    PeerCallPendingBytes,
    PeerCallNextEvent,
    PeerCallTxBytes,
    PeerCallCopyTx,
    PeerCallTick,
    PeerCallTransportLost,
    PeerCallDestroy
};

extern int __real_interop_peer_ack_tx(interop_peer_t *peer, uint64_t token,
                                      uint64_t now_ms, size_t written,
                                      interop_peer_event_t *event);
extern int __wrap_interop_peer_ack_tx(interop_peer_t *peer, uint64_t token,
                                      uint64_t now_ms, size_t written,
                                      interop_peer_event_t *event);
extern int __real_interop_peer_create(const interop_peer_config_t *config,
                                      interop_peer_t **out_peer);
extern int __wrap_interop_peer_create(const interop_peer_config_t *config,
                                      interop_peer_t **out_peer);
extern int __real_interop_peer_get_state(const interop_peer_t *peer, uint64_t token,
                                         interop_peer_event_t *event);
extern int __wrap_interop_peer_get_state(const interop_peer_t *peer, uint64_t token,
                                         interop_peer_event_t *event);
extern int __real_interop_peer_feed(interop_peer_t *peer, uint64_t token,
                                    const uint8_t *bytes, size_t length,
                                    size_t *consumed, interop_peer_event_t *event);
extern int __wrap_interop_peer_feed(interop_peer_t *peer, uint64_t token,
                                    const uint8_t *bytes, size_t length,
                                    size_t *consumed, interop_peer_event_t *event);
extern int __real_interop_peer_next_event(interop_peer_t *peer, uint64_t token,
                                          uint64_t now_ms, uint8_t *bytes,
                                          size_t capacity, interop_peer_event_t *event);
extern int __wrap_interop_peer_next_event(interop_peer_t *peer, uint64_t token,
                                          uint64_t now_ms, uint8_t *bytes,
                                          size_t capacity, interop_peer_event_t *event);
extern int __real_interop_peer_tx_bytes(const interop_peer_t *peer, uint64_t token,
                                        size_t *length);
extern int __wrap_interop_peer_tx_bytes(const interop_peer_t *peer, uint64_t token,
                                        size_t *length);
extern int __real_interop_peer_copy_tx(const interop_peer_t *peer, uint64_t token,
                                       uint8_t *bytes, size_t capacity, size_t *length);
extern int __wrap_interop_peer_copy_tx(const interop_peer_t *peer, uint64_t token,
                                       uint8_t *bytes, size_t capacity, size_t *length);
extern int __real_interop_peer_tick(interop_peer_t *peer, uint64_t token,
                                    uint64_t now_ms, interop_peer_event_t *event);
extern int __wrap_interop_peer_tick(interop_peer_t *peer, uint64_t token,
                                    uint64_t now_ms, interop_peer_event_t *event);
extern int __real_interop_peer_transport_lost(interop_peer_t *peer, uint64_t token,
                                              interop_peer_event_t *event);
extern int __wrap_interop_peer_transport_lost(interop_peer_t *peer, uint64_t token,
                                              interop_peer_event_t *event);
extern int __real_interop_peer_destroy(interop_peer_t **peer);
extern int __wrap_interop_peer_destroy(interop_peer_t **peer);
extern int __real_interop_peer_mapping_result(interop_peer_t *peer, uint64_t token,
                                              uint64_t now_ms, uint64_t epoch,
                                              uint32_t layout_version, uint32_t succeeded,
                                              interop_peer_event_t *event);
extern int __wrap_interop_peer_mapping_result(interop_peer_t *peer, uint64_t token,
                                              uint64_t now_ms, uint64_t epoch,
                                              uint32_t layout_version, uint32_t succeeded,
                                              interop_peer_event_t *event);
extern int __real_interop_peer_begin_drain(interop_peer_t *peer, uint64_t token,
                                           uint64_t now_ms, interop_peer_event_t *event);
extern int __wrap_interop_peer_begin_drain(interop_peer_t *peer, uint64_t token,
                                           uint64_t now_ms, interop_peer_event_t *event);
extern int __real_close(int fd);
extern int __wrap_close(int fd);
extern int __real_getsockopt(int fd, int level, int option_name,
                             void *option_value, socklen_t *option_length);
extern int __wrap_getsockopt(int fd, int level, int option_name,
                             void *option_value, socklen_t *option_length);
extern int __real_interop_peer_pending_bytes(interop_peer_t *peer, uint64_t token,
                                             size_t *length);
extern int __wrap_interop_peer_pending_bytes(interop_peer_t *peer, uint64_t token,
                                             size_t *length);
extern ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);
extern ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags);
extern ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
extern ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags);
extern void *__real_calloc(size_t count, size_t size);
extern void *__wrap_calloc(size_t count, size_t size);
extern int __real_fcntl(int fd, int command, ...);
extern int __wrap_fcntl(int fd, int command, ...);

int __wrap_interop_peer_ack_tx(interop_peer_t *peer, uint64_t token,
                               uint64_t now_ms, size_t written,
                               interop_peer_event_t *event)
{
    int result;
    if (fail_next_ack != 0) {
        fail_next_ack = 0;
        return InteropPeerInternalInvariant;
    }
    result = __real_interop_peer_ack_tx(peer, token, now_ms, written, event);
    if (result == InteropPeerSuccess && override_ack_event_phase >= 0) {
        event->phase = (uint32_t)override_ack_event_phase;
        override_ack_event_phase = -1;
    }
    return result;
}

int __wrap_interop_peer_create(const interop_peer_config_t *config,
                               interop_peer_t **out_peer)
{
    if (forced_peer_create_result >= 0)
        return forced_peer_create_result;
    return __real_interop_peer_create(config, out_peer);
}

int __wrap_interop_peer_get_state(const interop_peer_t *peer, uint64_t token,
                                  interop_peer_event_t *event)
{
    int result;
    if (fail_peer_call == PeerCallGetState) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    result = __real_interop_peer_get_state(peer, token, event);
    if (result == InteropPeerSuccess && override_get_state_phase >= 0) {
        event->phase = (uint32_t)override_get_state_phase;
        override_get_state_phase = -1;
    }
    return result;
}

int __wrap_interop_peer_feed(interop_peer_t *peer, uint64_t token,
                             const uint8_t *bytes, size_t length,
                             size_t *consumed, interop_peer_event_t *event)
{
    if (fail_peer_call == PeerCallFeed) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    {
        int result = __real_interop_peer_feed(peer, token, bytes, length, consumed, event);
        if (result == InteropPeerSuccess && override_feed_phase >= 0) {
            event->phase = (uint32_t)override_feed_phase;
            override_feed_phase = -1;
        }
        if (report_oversized_feed_consumed != 0 && result == InteropPeerSuccess) {
            report_oversized_feed_consumed = 0;
            *consumed = length + 1;
        } else if (report_zero_feed_consumed != 0 && result == InteropPeerSuccess) {
            report_zero_feed_consumed = 0;
            *consumed = 0;
        }
        return result;
    }
}

int __wrap_interop_peer_pending_bytes(interop_peer_t *peer, uint64_t token,
                                      size_t *length)
{
    if (fail_peer_call == PeerCallPendingBytes) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    if (suppress_pending_after_recv != 0) {
        suppress_pending_after_recv = 0;
        *length = 0;
        return InteropPeerSuccess;
    }
    return __real_interop_peer_pending_bytes(peer, token, length);
}

int __wrap_interop_peer_next_event(interop_peer_t *peer, uint64_t token,
                                   uint64_t now_ms, uint8_t *bytes,
                                   size_t capacity, interop_peer_event_t *event)
{
    int result;
    ++peer_next_event_calls;
    if (fail_peer_call == PeerCallNextEvent) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    result = __real_interop_peer_next_event(peer, token, now_ms, bytes, capacity, event);
    if (result == InteropPeerSuccess && override_next_event_phase >= 0) {
        event->phase = (uint32_t)override_next_event_phase;
        override_next_event_phase = -1;
    }
    if (result == InteropPeerSuccess && force_abort_next_event != 0) {
        force_abort_next_event = 0;
        event->flags |= InteropPeerAbortTransport;
    }
    return result;
}

int __wrap_interop_peer_tx_bytes(const interop_peer_t *peer, uint64_t token,
                                 size_t *length)
{
    if (fail_peer_call == PeerCallTxBytes) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    return __real_interop_peer_tx_bytes(peer, token, length);
}

int __wrap_interop_peer_copy_tx(const interop_peer_t *peer, uint64_t token,
                                uint8_t *bytes, size_t capacity, size_t *length)
{
    int result;
    if (fail_peer_call == PeerCallCopyTx) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    result = __real_interop_peer_copy_tx(peer, token, bytes, capacity, length);
    if (result == InteropPeerSuccess && override_copy_tx_length != 0) {
        if (override_copy_tx_length == 1)
            *length = 0;
        else
            *length += 1;
        override_copy_tx_length = 0;
    }
    return result;
}

int __wrap_interop_peer_tick(interop_peer_t *peer, uint64_t token,
                             uint64_t now_ms, interop_peer_event_t *event)
{
    int result;
    if (fail_peer_call == PeerCallTick) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    result = __real_interop_peer_tick(peer, token, now_ms, event);
    if (result == InteropPeerSuccess && override_tick_phase >= 0) {
        event->phase = (uint32_t)override_tick_phase;
        override_tick_phase = -1;
    }
    return result;
}

int __wrap_interop_peer_transport_lost(interop_peer_t *peer, uint64_t token,
                                       interop_peer_event_t *event)
{
    if (fail_transport_lost_count != 0) {
        --fail_transport_lost_count;
        return InteropPeerInternalInvariant;
    }
    if (fail_peer_call == PeerCallTransportLost) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    return __real_interop_peer_transport_lost(peer, token, event);
}

int __wrap_interop_peer_destroy(interop_peer_t **peer)
{
    if (fail_peer_call == PeerCallDestroy) {
        fail_peer_call = PeerCallNone;
        return InteropPeerInternalInvariant;
    }
    return __real_interop_peer_destroy(peer);
}

int __wrap_interop_peer_mapping_result(interop_peer_t *peer, uint64_t token,
                                       uint64_t now_ms, uint64_t epoch,
                                       uint32_t layout_version, uint32_t succeeded,
                                       interop_peer_event_t *event)
{
    if (forced_mapping_result >= 0) {
        int result = forced_mapping_result;
        forced_mapping_result = -1;
        return result;
    }
    if (fail_next_mapping != 0) {
        fail_next_mapping = 0;
        return InteropPeerInternalInvariant;
    }
    {
        int result = __real_interop_peer_mapping_result(peer, token, now_ms, epoch,
                                                        layout_version, succeeded, event);
        if (result == InteropPeerSuccess && override_mapping_phase >= 0) {
            event->phase = (uint32_t)override_mapping_phase;
            override_mapping_phase = -1;
        }
        return result;
    }
}

int __wrap_interop_peer_begin_drain(interop_peer_t *peer, uint64_t token,
                                    uint64_t now_ms, interop_peer_event_t *event)
{
    if (forced_drain_result >= 0) {
        int result = forced_drain_result;
        forced_drain_result = -1;
        return result;
    }
    if (fail_next_drain != 0) {
        fail_next_drain = 0;
        return InteropPeerInternalInvariant;
    }
    {
        int result = __real_interop_peer_begin_drain(peer, token, now_ms, event);
        if (result == InteropPeerSuccess && override_drain_phase >= 0) {
            event->phase = (uint32_t)override_drain_phase;
            override_drain_phase = -1;
        }
        return result;
    }
}

int __wrap_close(int fd)
{
    ++total_close_calls;
    if (fail_next_close != 0) {
        fail_next_close = 0;
        ++wrapped_close_calls;
        (void)__real_close(fd); /* model EINTR after the descriptor was released */
        errno = EINTR;
        return -1;
    }
    return __real_close(fd);
}

int __wrap_getsockopt(int fd, int level, int option_name,
                      void *option_value, socklen_t *option_length)
{
    if (level == SOL_SOCKET && option_name == SO_ERROR && fail_next_so_error_errno != 0) {
        int saved_error = fail_next_so_error_errno;
        fail_next_so_error_errno = 0;
        errno = saved_error;
        return -1;
    }
    if (fail_next_so_error != 0 && level == SOL_SOCKET && option_name == SO_ERROR) {
        fail_next_so_error = 0;
        errno = EINTR;
        return -1;
    }
    if (level == SOL_SOCKET && option_name == SO_ERROR && injected_so_error != 0) {
        if (option_value == NULL || option_length == NULL ||
            *option_length < (socklen_t)sizeof(int))
            abort();
        *(int *)option_value = injected_so_error;
        *option_length = (socklen_t)sizeof(int);
        injected_so_error = 0;
        return 0;
    }
    return __real_getsockopt(fd, level, option_name, option_value, option_length);
}

ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags)
{
    ssize_t received;
    if (recv_chunk_schedule_calls != 0) {
        size_t scheduled_limit = recv_chunk_two_byte_calls != 0 ? 2u : 1u;
        if (recv_chunk_two_byte_calls != 0)
            --recv_chunk_two_byte_calls;
        --recv_chunk_schedule_calls;
        if (length > scheduled_limit)
            length = scheduled_limit;
    } else if (recv_chunk_limit != 0 && length > recv_chunk_limit) {
        length = recv_chunk_limit;
    }
    if (fail_next_recv_errno != 0) {
        int saved_error = fail_next_recv_errno;
        fail_next_recv_errno = 0;
        errno = saved_error;
        return -1;
    }
    if (return_zero_recv != 0) {
        return_zero_recv = 0;
        return 0;
    }
    received = __real_recv(fd, buffer, length, flags);
    if (received > 0 && fail_feed_after_recv != 0) {
        fail_feed_after_recv = 0;
        fail_peer_call = PeerCallFeed;
    }
    if (received > 0 && fail_pending_after_recv != 0) {
        fail_pending_after_recv = 0;
        fail_peer_call = PeerCallPendingBytes;
    }
    if (received > 0 && arm_pending_suppression_after_recv != 0) {
        arm_pending_suppression_after_recv = 0;
        suppress_pending_after_recv = 1;
    }
    return received;
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags)
{
    if (fail_next_send_errno != 0) {
        int saved_error = fail_next_send_errno;
        fail_next_send_errno = 0;
        errno = saved_error;
        return -1;
    }
    if (return_zero_send != 0) {
        return_zero_send = 0;
        return 0;
    }
    return __real_send(fd, buffer, length, flags);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_next_calloc != 0) {
        fail_next_calloc = 0;
        return NULL;
    }
    return __real_calloc(count, size);
}

int __wrap_fcntl(int fd, int command, ...)
{
    va_list arguments;
    if (command == F_GETFL && fail_next_getfl != 0) {
        fail_next_getfl = 0;
        errno = EIO;
        return -1;
    }
    if (command == F_GETFD && fail_next_getfd != 0) {
        fail_next_getfd = 0;
        errno = EIO;
        return -1;
    }
    if (command == F_GETFL || command == F_GETFD)
        return __real_fcntl(fd, command);
    if (command == F_SETFL || command == F_SETFD) {
        int value;
        va_start(arguments, command);
        value = va_arg(arguments, int);
        va_end(arguments);
        return __real_fcntl(fd, command, value);
    }
    if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
        struct flock *lock;
        va_start(arguments, command);
        lock = va_arg(arguments, struct flock *);
        va_end(arguments);
        return __real_fcntl(fd, command, lock);
    }
    fprintf(stderr, "unexpected fcntl command=%d\n", command);
    abort();
}

static void fail_at(const char *expression, int line)
{
    fprintf(stderr, "linux_socket_owner.c:%d: check failed: %s\n", line, expression);
    exit(1);
}

#define Check(expression) do { if (!(expression)) fail_at(#expression, __LINE__); } while (0)

static int bytes_are(const void *storage, size_t length, uint8_t expected)
{
    const uint8_t *bytes = (const uint8_t *)storage;
    size_t index;
    for (index = 0; index < length; ++index) {
        if (bytes[index] != expected)
            return 0;
    }
    return 1;
}

static interop_linux_socket_config_t make_config(uint32_t role, uint64_t token)
{
    static const uint8_t Version[] = "1.2.3";
    interop_linux_socket_config_t config;

    memset(&config, 0, sizeof(config));
    config.peer_config.role = role;
    config.peer_config.connection_token = token;
    config.peer_config.accepted_at_ms = 100;
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1;
    config.peer_config.local_capabilities = InteropCapWindows | InteropCapInput;
    config.drain_timeout_ms = 500;
    return config;
}

static void create_pair(int pair[2])
{
    Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
}

static interop_linux_socket_t *create_owner(int fd, uint32_t role, uint64_t token)
{
    interop_linux_socket_config_t config = make_config(role, token);
    interop_linux_socket_t *owner = NULL;
    Check(interop_linux_socket_create(fd, &config, &owner) == InteropLinuxSocketSuccess);
    Check(owner != NULL);
    return owner;
}

static int poll_drive(interop_linux_socket_t *owner, uint64_t token,
                      uint64_t now_ms, size_t rx_budget, size_t tx_budget,
                      interop_linux_socket_work_t *work)
{
    interop_linux_socket_poll_t poll_state;
    struct pollfd descriptor;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    int poll_result;

    memset(&poll_state, 0xa5, sizeof(poll_state));
    Check(interop_linux_socket_poll_events(owner, token, now_ms, &poll_state) == InteropLinuxSocketSuccess);
    descriptor.fd = poll_state.fd;
    descriptor.events = (short)poll_state.events;
    descriptor.revents = 0;
    poll_result = poll(&descriptor, 1, 0);
    Check(poll_result >= 0);
    memset(frame, 0xCC, sizeof(frame));
    return interop_linux_socket_drive(owner, token, poll_state.poll_cookie,
                                      now_ms, (uint32_t)(unsigned short)descriptor.revents,
                                      rx_budget, tx_budget, 16, frame, sizeof(frame), work);
}

/** @brief Encode one protocol frame carrying a single unsigned field. */
static size_t make_uint_frame(uint8_t *frame, size_t capacity, uint16_t type,
                              uint32_t request_id, interop_direction_t direction,
                              uint16_t tag, uint64_t value)
{
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, tag, value) == InteropErrorNone);
    Check(interop_writer_finish(&writer, type, request_id, direction,
                                &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode a valid empty control frame such as a Ping or Pong. */
static size_t make_empty_frame(uint8_t *frame, size_t capacity, uint16_t type,
                               uint32_t request_id, interop_direction_t direction)
{
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_writer_finish(&writer, type, request_id, direction,
                                &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode one structurally valid Guest Hello for ready-phase tests. */
static size_t make_hello_frame(uint8_t *frame, size_t capacity,
                               uint32_t request_id, uint64_t epoch)
{
    static const uint8_t Version[] = "1.2.3";
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, InteropTagCapabilities,
                           InteropCapWindows | InteropCapInput) == InteropErrorNone);
    Check(interop_put_bytes(&writer, InteropTagAgentVersion, Version,
                            sizeof(Version) - 1) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagLayoutVersion,
                           InteropLayoutVersion) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagSessionEpoch, epoch) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgHello, request_id,
                                InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode one structurally valid Host HelloAck for bounded-send tests. */
static size_t make_hello_ack_frame(uint8_t *frame, size_t capacity,
                                   uint32_t request_id)
{
    static const uint8_t Version[] = "1.2.3";
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, InteropTagCapabilities,
                           InteropCapWindows | InteropCapInput) == InteropErrorNone);
    Check(interop_put_bytes(&writer, InteropTagAgentVersion, Version,
                            sizeof(Version) - 1) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagLayoutVersion,
                           InteropLayoutVersion) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgHelloAck, request_id,
                                InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

static void test_descriptor_transfer_and_preflight(void)
{
    int pair[2];
    int pipe_fds[2];
    interop_linux_socket_config_t config;
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_t *sentinel = (interop_linux_socket_t *)(uintptr_t)0x1234;

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x101);
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);
    Check(fcntl(pair[0], F_GETFD) >= 0);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(owner == NULL);
    errno = 0;
    Check(fcntl(pair[0], F_GETFD) == -1 && errno == EBADF);
    Check(close(pair[1]) == 0);

    Check(pipe(pipe_fds) == 0);
    config = make_config(InteropPeerHost, 0x102);
    owner = NULL;
    Check(interop_linux_socket_create(pipe_fds[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    Check(owner == NULL && fcntl(pipe_fds[0], F_GETFD) >= 0);
    Check(close(pipe_fds[0]) == 0);
    Check(close(pipe_fds[1]) == 0);

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x103);
    Check(interop_linux_socket_create(pair[0], &config, &sentinel) == InteropLinuxSocketInvalidArgument);
    Check(sentinel == (interop_linux_socket_t *)(uintptr_t)0x1234);
    Check(fcntl(pair[0], F_GETFD) >= 0);
    Check(close(pair[0]) == 0);
    Check(close(pair[1]) == 0);
}

static void test_create_preflight_and_peer_result_mapping(void)
{
    static const struct {
        int peer_result;
        int owner_result;
    } PeerFailures[] = {
        { InteropPeerInvalidArgument, InteropLinuxSocketInvalidArgument },
        { InteropPeerOutOfMemory, InteropLinuxSocketOutOfMemory },
        { InteropPeerWrongToken, InteropLinuxSocketPeerFailure },
        { InteropPeerTimeWentBackwards, InteropLinuxSocketPeerFailure },
        { InteropPeerWrongPhase, InteropLinuxSocketPeerFailure },
        { InteropPeerMappingFailed, InteropLinuxSocketPeerFailure },
        { InteropPeerClosed, InteropLinuxSocketPeerFailure },
        { 99, InteropLinuxSocketPeerFailure }
    };
    int pair[2];
    interop_linux_socket_config_t config;
    interop_linux_socket_t *owner = NULL;
    size_t index;

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x110);
    Check(interop_linux_socket_create(pair[0], NULL, &owner) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_create(pair[0], &config, NULL) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_create(-1, &config, &owner) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_create(pair[0], &config,
          (interop_linux_socket_t **)(void *)((uint8_t *)&owner + 1)) ==
          InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_create(pair[0], (const interop_linux_socket_config_t *)(uintptr_t)UINTPTR_MAX,
          &owner) == InteropLinuxSocketInvalidArgument);

    config.reserved_zero = 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    config.reserved_zero = 0;
    config.drain_timeout_ms = 0;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    config.drain_timeout_ms = InteropLinuxSocketMaxDrainTimeoutMs + 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    config = make_config(InteropPeerHost, 0x110);
    config.peer_config.agent_version = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u);
    config.peer_config.agent_version_bytes = 8;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    config = make_config(InteropPeerHost, 0x110);
    config.peer_config.agent_version = (const uint8_t *)&owner;
    config.peer_config.agent_version_bytes = 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    config = make_config(InteropPeerHost, 0x110);
    config.peer_config.agent_version = (const uint8_t *)&owner;
    config.peer_config.agent_version_bytes = 0;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    Check(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);

    for (index = 0; index < sizeof(PeerFailures) / sizeof(PeerFailures[0]); ++index) {
        config = make_config(InteropPeerHost, 0x120 + (uint64_t)index);
        forced_peer_create_result = PeerFailures[index].peer_result;
        Check(interop_linux_socket_create(pair[0], &config, &owner) == PeerFailures[index].owner_result);
        Check(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    }
    forced_peer_create_result = -1;
    Check(close(pair[0]) == 0 && close(pair[1]) == 0);
}

static void test_owner_allocation_failure_preserves_fd(void)
{
    int pair[2];
    interop_linux_socket_config_t config;
    interop_linux_socket_t *owner = NULL;

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x10F);
    fail_next_calloc = 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketOutOfMemory);
    Check(fail_next_calloc == 0 && owner == NULL);
    Check(fcntl(pair[0], F_GETFD) >= 0);
    Check(close(pair[0]) == 0 && close(pair[1]) == 0);
}

static void test_descriptor_flag_probe_failures_preserve_fd(void)
{
    int pair[2];
    interop_linux_socket_config_t config;
    interop_linux_socket_t *owner = NULL;

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x10E);
    fail_next_getfl = 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    Check(fail_next_getfl == 0 && owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    Check(close(pair[0]) == 0 && close(pair[1]) == 0);

    create_pair(pair);
    config = make_config(InteropPeerHost, 0x10D);
    fail_next_getfd = 1;
    Check(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketInvalidArgument);
    Check(fail_next_getfd == 0 && owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    Check(close(pair[0]) == 0 && close(pair[1]) == 0);
}

static void test_poll_and_tick_peer_faults_fail_closed(void)
{
    static const int PollFaults[] = {
        PeerCallGetState,
        PeerCallTxBytes,
        PeerCallPendingBytes
    };
    size_t index;

    for (index = 0; index < sizeof(PollFaults) / sizeof(PollFaults[0]); ++index) {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint64_t token = 0x130 + (uint64_t)index;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, token);
        memset(&poll_state, 0xA5, sizeof(poll_state));
        fail_peer_call = PollFaults[index];
        Check(interop_linux_socket_poll_events(owner, token, 100, &poll_state) ==
              InteropLinuxSocketPeerFailure);
        Check(fail_peer_call == PeerCallNone &&
              bytes_are(&poll_state, sizeof(poll_state), 0xA5));
        Check(interop_linux_socket_get_closed_snapshot(owner, token, &work) ==
              InteropLinuxSocketSuccess);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x140;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_peer_call = PeerCallTick;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }
}

static void test_receive_faults_and_peer_event_fault_close(void)
{
    static const int ReceiveFaults[] = { PeerCallFeed, PeerCallPendingBytes };
    size_t index;

    for (index = 0; index < sizeof(ReceiveFaults) / sizeof(ReceiveFaults[0]); ++index) {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x150 + (uint64_t)index;
        const uint8_t Input[] = { 0x57 };

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], Input, sizeof(Input), MSG_NOSIGNAL) == (ssize_t)sizeof(Input));
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        if (ReceiveFaults[index] == PeerCallFeed)
            fail_feed_after_recv = 1;
        else
            fail_pending_after_recv = 1;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 4, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        uint8_t input = 0x33;
        const uint64_t Token = 0x198;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], &input, sizeof(input), MSG_NOSIGNAL) == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_next_recv_errno = EAGAIN;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 4, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(fail_next_recv_errno == 0 && work.rx_bytes == 0);
        Check(work.state == InteropLinuxSocketActive &&
              work.status != InteropLinuxSocketWorkTransportClosed);
        Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint8_t Input = 0x32;
        const uint64_t Token = 0x15F;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], &Input, sizeof(Input), MSG_NOSIGNAL) == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        report_oversized_feed_consumed = 1;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 4, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(report_oversized_feed_consumed == 0);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint8_t Input = 0x32;
        const uint64_t Token = 0x15E;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], &Input, sizeof(Input), MSG_NOSIGNAL) == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        report_zero_feed_consumed = 1;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 4, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(report_zero_feed_consumed == 0);
        Check(work.state == InteropLinuxSocketActive && work.rx_bytes == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        Check(poll_state.immediate_work == 1);
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.state == InteropLinuxSocketActive && work.event.kind == InteropPeerEventNone);
        Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x197;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x1970,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_next_send_errno = EIO;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalSocketError);
        Check(work.system_errno == EIO);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t HostToken = 0x160;
        const uint64_t GuestToken = 0x161;
        interop_linux_socket_t *host;
        interop_linux_socket_t *guest;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        host = create_owner(pair[0], InteropPeerHost, HostToken);
        guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
        Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x1600,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_drive(guest, GuestToken, poll_state.poll_cookie,
              100, POLLOUT, 0, InteropLinuxSocketFrameCapacity, 8,
              frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_peer_call = PeerCallNextEvent;
        Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
              100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0, 8,
              frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    }
}

static void test_tx_faults_and_destroy_retries(void)
{
    static const int TxFaults[] = { PeerCallTxBytes, PeerCallCopyTx, PeerCallGetState };
    size_t index;

    for (index = 0; index < sizeof(TxFaults) / sizeof(TxFaults[0]); ++index) {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        uint64_t token = 0x170 + (uint64_t)index;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, token);
        Check(interop_linux_socket_mapping_result(owner, token, 100, 0x1700 + index,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_peer_call = TxFaults[index];
        Check(interop_linux_socket_drive(owner, token, poll_state.poll_cookie,
              100, POLLOUT, 0, 32, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        if (TxFaults[index] == PeerCallGetState)
            Check(work.tx_bytes != 0);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_work_t work;
        uint64_t token = 0x180;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, token);
        fail_transport_lost_count = 2;
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketPeerFailure);
        Check(owner != NULL);
        Check(interop_linux_socket_get_closed_snapshot(owner, token, &work) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        uint64_t token = 0x181;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, token);
        fail_peer_call = PeerCallDestroy;
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketPeerFailure);
        Check(owner != NULL);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(owner == NULL && close(pair[1]) == 0);
    }
}

static void test_copy_tx_success_with_invalid_length_fails_closed(void)
{
    int mismatch;

    for (mismatch = 1; mismatch <= 2; ++mismatch) {
        int pair[2];
        const uint64_t Token = 0x182u + (uint64_t)mismatch;
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x1820u +
              (uint64_t)mismatch, InteropLayoutVersion, 1, &event) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_copy_tx_length = mismatch;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, InteropLinuxSocketFrameCapacity, 8,
              frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
        Check(override_copy_tx_length == 0);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.tx_bytes == 0 && work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }
}

static void test_draining_tx_query_failure_is_terminal(void)
{
    int pair[2];
    const uint64_t Token = 0x184;
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    Check(interop_linux_socket_begin_drain(owner, Token, 100, &event) ==
          InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    fail_peer_call = PeerCallTxBytes;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, 0, 0, 0, 8, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(fail_peer_call == PeerCallNone);
    Check(work.status == InteropLinuxSocketWorkPeerFailure);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_socket_syscall_fault_paths(void)
{
    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x190;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x1900,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_next_send_errno = EAGAIN;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.tx_bytes == 0 && owner != NULL);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        Check(poll_state.immediate_work == 0 && (poll_state.events & POLLOUT) != 0);
        fail_next_send_errno = EINTR;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.syscalls >= 2 && work.tx_bytes != 0);
        Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x191;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x1910,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        return_zero_send = 1;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalSocketError);
        Check(work.system_errno == EPIPE);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x192;
        const uint8_t Input = 0x57;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], &Input, sizeof(Input), MSG_NOSIGNAL) == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_next_recv_errno = EINTR;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.rx_bytes == 1 && work.syscalls >= 2);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x193;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_next_recv_errno = EIO;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, 4, 0, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalSocketError);
        Check(work.system_errno == EIO);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        static const int SoErrors[] = { ECONNRESET, EIO };
        size_t index;

        for (index = 0; index < sizeof(SoErrors) / sizeof(SoErrors[0]); ++index) {
            int pair[2];
            interop_linux_socket_t *owner;
            interop_linux_socket_poll_t poll_state;
            interop_linux_socket_work_t work;
            uint8_t frame[InteropLinuxSocketFrameCapacity];
            uint64_t token = 0x194 + (uint64_t)index;

            create_pair(pair);
            owner = create_owner(pair[0], InteropPeerHost, token);
            Check(interop_linux_socket_poll_events(owner, token, 100, &poll_state) ==
                  InteropLinuxSocketSuccess);
            if (index == 0)
                injected_so_error = SoErrors[index];
            else
                fail_next_so_error_errno = SoErrors[index];
            fail_transport_lost_count = 1;
            Check(interop_linux_socket_drive(owner, token, poll_state.poll_cookie,
                  100, POLLERR, 0, 0, 0, frame, sizeof(frame), &work) ==
                  InteropLinuxSocketSuccess);
            Check(work.state == InteropLinuxSocketTransportClosed);
            Check(work.terminal_reason == InteropLinuxSocketTerminalSocketError);
            Check(work.system_errno == SoErrors[index]);
            Check(work.status == InteropLinuxSocketWorkPeerFailure);
            Check(fail_transport_lost_count == 0);
            Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
            Check(close(pair[1]) == 0);
        }
    }

    {
        int pair[2];
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        const uint64_t Token = 0x196;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLNVAL, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPollInvalid);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x212;
        interop_linux_socket_t *owner;
        interop_linux_socket_work_t work;
        interop_linux_socket_work_t snapshot;
        interop_linux_socket_poll_t poll_state;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        total_close_calls = 0;
        fail_transport_lost_count = 1;
        Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
        Check(fail_transport_lost_count == 0);
        Check(total_close_calls == 1);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        /* A repeated abort is a stable closed snapshot, not another failed
         * notification attempt or a replay of the original operation status. */
        fail_transport_lost_count = 1;
        Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
        Check(work.status == InteropLinuxSocketWorkTransportClosed);
        Check(work.event.kind == InteropPeerEventNone && work.internal_steps == 0);
        Check(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
        Check(total_close_calls == 1 && fail_transport_lost_count == 1);
        Check(interop_linux_socket_get_closed_snapshot(owner, Token, &snapshot) ==
              InteropLinuxSocketSuccess);
        Check(memcmp(&work, &snapshot, sizeof(work)) == 0);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketPeerFailure);
        Check(owner != NULL && fail_transport_lost_count == 0 && total_close_calls == 1);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }
}

static void test_guest_mapping_preflight_and_abort(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_work_t work;
    interop_linux_socket_poll_t poll_state;
    const uint64_t Token = 0x204;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    memset(&event, 0xA5, sizeof(event));
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 42,
          InteropLayoutVersion, 0, &event) == InteropLinuxSocketMappingFailed);
    Check(bytes_are(&event, sizeof(event), 0xA5));
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketInvalidArgument);
    Check(bytes_are(&event, sizeof(event), 0xA5));
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 42,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventNone);
    memset(&event, 0xB6, sizeof(event));
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 43,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketWrongPhase);
    Check(bytes_are(&event, sizeof(event), 0xB6));

    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) == InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0 && poll_state.immediate_work == 0);
    memset(&work, 0xA5, sizeof(work));
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, 0, 0, 0, 0, (uint8_t *)&work, sizeof(work), &work) == InteropLinuxSocketInvalidArgument);
    Check(bytes_are(&work, sizeof(work), 0xA5));

    Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) == InteropLinuxSocketSuccess);
    Check(poll_state.fd == -1 && poll_state.events == 0 && poll_state.timeout_ms == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, 0, 0, 0, 0, (uint8_t[InteropLinuxSocketFrameCapacity]){0},
          InteropLinuxSocketFrameCapacity, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);
    Check(interop_linux_socket_get_closed_snapshot(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(owner == NULL);
    Check(close(pair[1]) == 0);
}

static void test_non_drive_peer_invariants_fail_closed_and_preserve_output(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_work_t work;
    const uint64_t Token = 0x205;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    memset(&event, 0xA5, sizeof(event));
    fail_next_mapping = 1;
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 42,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketPeerFailure);
    Check(fail_next_mapping == 0 && bytes_are(&event, sizeof(event), 0xA5));
    Check(interop_linux_socket_get_closed_snapshot(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(owner == NULL && close(pair[1]) == 0);

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token + 1);
    memset(&event, 0x5A, sizeof(event));
    fail_next_drain = 1;
    Check(interop_linux_socket_begin_drain(owner, Token + 1, 100, &event) ==
          InteropLinuxSocketPeerFailure);
    Check(fail_next_drain == 0 && bytes_are(&event, sizeof(event), 0x5A));
    Check(interop_linux_socket_get_closed_snapshot(owner, Token + 1, &work) ==
          InteropLinuxSocketSuccess);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(owner == NULL && close(pair[1]) == 0);

    {
        static const int EscapedResults[] = {
            InteropPeerWrongPhase,
            InteropPeerInvalidArgument,
            InteropPeerMappingFailed
        };
        size_t index;

        for (index = 0; index < sizeof(EscapedResults) / sizeof(EscapedResults[0]); ++index) {
            uint64_t local_token = Token + 2u + (uint64_t)index;
            create_pair(pair);
            owner = create_owner(pair[0], InteropPeerGuest, local_token);
            memset(&event, 0xC3, sizeof(event));
            forced_mapping_result = EscapedResults[index];
            Check(interop_linux_socket_mapping_result(owner, local_token, 100,
                  0x2200 + (uint64_t)index, InteropLayoutVersion, 1, &event) ==
                  InteropLinuxSocketPeerFailure);
            Check(forced_mapping_result == -1 && bytes_are(&event, sizeof(event), 0xC3));
            Check(interop_linux_socket_get_closed_snapshot(owner, local_token, &work) ==
                  InteropLinuxSocketSuccess);
            Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
            Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
            Check(close(pair[1]) == 0);
        }
    }

    {
        static const int EscapedResults[] = {
            InteropPeerWrongPhase,
            InteropPeerInvalidArgument
        };
        size_t index;

        for (index = 0; index < sizeof(EscapedResults) / sizeof(EscapedResults[0]); ++index) {
            uint64_t local_token = Token + 5u + (uint64_t)index;
            create_pair(pair);
            owner = create_owner(pair[0], InteropPeerHost, local_token);
            memset(&event, 0xD4, sizeof(event));
            forced_drain_result = EscapedResults[index];
            Check(interop_linux_socket_begin_drain(owner, local_token, 100, &event) ==
                  InteropLinuxSocketPeerFailure);
            Check(forced_drain_result == -1 && bytes_are(&event, sizeof(event), 0xD4));
            Check(interop_linux_socket_get_closed_snapshot(owner, local_token, &work) ==
                  InteropLinuxSocketSuccess);
            Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
            Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
            Check(close(pair[1]) == 0);
        }
    }
}

static void test_host_mapping_failure_is_forwarded_and_deadline_is_fixed(void)
{
    int pair[2];
    const uint64_t HostToken = 0x206;
    const uint64_t GuestToken = 0x207;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t map_event;
    interop_peer_event_t result_event;
    interop_linux_socket_work_t work;
    interop_linux_socket_poll_t poll_state;
    uint64_t now_ms = 100;
    unsigned index;

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, now_ms, 0x5555,
          InteropLayoutVersion, 1, &result_event) == InteropLinuxSocketSuccess);
    memset(&map_event, 0, sizeof(map_event));
    for (index = 0; index < 256 && map_event.kind != InteropPeerEventValidateMapping; ++index) {
        ++now_ms;
        Check(poll_drive(guest, GuestToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(host, HostToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
        if (work.event.kind == InteropPeerEventValidateMapping)
            map_event = work.event;
    }
    Check(map_event.kind == InteropPeerEventValidateMapping);
    /* Host mismatch is peer evidence: it is forwarded and begins Draining. */
    Check(interop_linux_socket_mapping_result(host, HostToken, now_ms,
          map_event.session_epoch + 1, map_event.layout_version + 1, 0,
          &result_event) == InteropLinuxSocketSuccess);
    Check(result_event.kind == InteropPeerEventDraining);
    Check(result_event.phase == InteropPeerDraining);
    Check(interop_linux_socket_begin_drain(host, HostToken, now_ms + 10,
          &result_event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(host, HostToken, now_ms + 410,
          &poll_state) == InteropLinuxSocketSuccess);
    Check(poll_state.timeout_ms == 90);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          now_ms + 410, POLLOUT, 0, InteropLinuxSocketMaxTxBudgetBytes,
          InteropLinuxSocketMaxSyscallBudget, (uint8_t[InteropLinuxSocketFrameCapacity]){0},
          InteropLinuxSocketFrameCapacity, &work) == InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0 && work.state == InteropLinuxSocketDraining);
    Check(interop_linux_socket_poll_events(host, HostToken, now_ms + 411,
          &poll_state) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          now_ms + 411, 0, 0, 0, 0, (uint8_t[InteropLinuxSocketFrameCapacity]){0},
          InteropLinuxSocketFrameCapacity, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainComplete);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
}

static void test_drain_deadline_closes_at_equality(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    const uint64_t Token = 0x208;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    Check(interop_linux_socket_begin_drain(owner, Token, 100, &event) ==
          InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining);
    Check(interop_linux_socket_poll_events(owner, Token, 600, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1 && poll_state.timeout_ms == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          600, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainDeadline);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_abort_tick_skips_fatal_socket_probe(void)
{
    int pair[2];
    const uint64_t Token = 0x20A;
    const uint64_t TimeoutAt = 100 + InteropHelloTimeoutMs;
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    {
        interop_peer_event_t event;
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x20A0,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    }
    Check(interop_linux_socket_poll_events(owner, Token, TimeoutAt, &poll_state) ==
          InteropLinuxSocketSuccess);
    fail_next_so_error = 1;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          TimeoutAt, POLLERR | POLLOUT, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(fail_next_so_error == 1);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerAbort);
    Check(work.status == InteropLinuxSocketWorkTransportClosed);
    Check(work.system_errno == 0);
    fail_next_so_error = 0;
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_pollnval_transport_notification_failure_is_reported(void)
{
    int pair[2];
    const uint64_t Token = 0x20B;
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    fail_transport_lost_count = 1;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLNVAL, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(fail_transport_lost_count == 0);
    Check(work.status == InteropLinuxSocketWorkPeerFailure);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPollInvalid);
    Check(interop_linux_socket_get_closed_snapshot(owner, Token, &work) ==
          InteropLinuxSocketSuccess);
    Check(work.status == InteropLinuxSocketWorkTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPollInvalid);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_unexpected_peer_phases_fail_closed_at_all_observation_sites(void)
{
    {
        int pair[2];
        const uint64_t Token = 0x20D;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t snapshot;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        memset(&poll_state, 0xA5, sizeof(poll_state));
        override_get_state_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketPeerFailure);
        Check(override_get_state_phase == -1);
        Check(bytes_are(&poll_state, sizeof(poll_state), 0xA5));
        Check(interop_linux_socket_get_closed_snapshot(owner, Token, &snapshot) ==
              InteropLinuxSocketSuccess);
        Check(snapshot.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x20E;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_tick_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(override_tick_phase == -1);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x20F;
        interop_linux_socket_t *owner;
        interop_peer_event_t event;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        memset(&event, 0xB7, sizeof(event));
        override_mapping_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x20F0,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketPeerFailure);
        Check(override_mapping_phase == -1);
        Check(bytes_are(&event, sizeof(event), 0xB7));
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x210;
        interop_linux_socket_t *owner;
        interop_peer_event_t event;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        memset(&event, 0xC8, sizeof(event));
        override_drain_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_begin_drain(owner, Token, 100, &event) ==
              InteropLinuxSocketPeerFailure);
        Check(override_drain_phase == -1);
        Check(bytes_are(&event, sizeof(event), 0xC8));
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x211;
        interop_linux_socket_t *owner;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerGuest, Token);
        Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x2110,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_get_state_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(override_get_state_phase == -1);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.tx_bytes != 0);
        Check(work.state == InteropLinuxSocketTransportClosed);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }
}

static void test_invalid_feed_next_event_and_ack_phases_fail_closed(void)
{
    {
        int pair[2];
        const uint64_t Token = 0x213;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t malformed_header[InteropHeaderBytes] = { 0 };
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], malformed_header, sizeof(malformed_header), MSG_NOSIGNAL) ==
              (ssize_t)sizeof(malformed_header));
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_feed_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
              InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(override_feed_phase == -1);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x214;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        interop_writer_t writer;
        uint8_t feature[InteropLinuxSocketFrameCapacity];
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        size_t feature_length = 0;

        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        interop_writer_init(&writer, feature, sizeof(feature));
        Check(interop_put_uint(&writer, InteropTagWindow, 7) == InteropErrorNone);
        Check(interop_put_uint(&writer, InteropTagGeneration, 2) == InteropErrorNone);
        Check(interop_writer_finish(&writer, InteropMsgWindowRetire, 1,
                                    InteropFromGuest, &feature_length) ==
              InteropErrorNone);
        Check(send(pair[1], feature, feature_length, MSG_NOSIGNAL) ==
              (ssize_t)feature_length);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_next_event_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
              InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(override_next_event_phase == -1);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.event.kind == InteropPeerEventNone);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t HostToken = 0x215;
        const uint64_t GuestToken = 0x216;
        interop_linux_socket_t *host;
        interop_linux_socket_t *guest;
        interop_peer_event_t event;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t frame[InteropLinuxSocketFrameCapacity];

        create_pair(pair);
        host = create_owner(pair[0], InteropPeerHost, HostToken);
        guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
        Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x2150,
              InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
        Check(poll_drive(guest, GuestToken, 101, 0, InteropLinuxSocketFrameCapacity,
              &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(host, HostToken, 101, InteropLinuxSocketMaxRxBudgetBytes,
              0, &work) == InteropLinuxSocketSuccess);
        Check(work.event.kind == InteropPeerEventValidateMapping);
        Check(interop_linux_socket_mapping_result(host, HostToken, 101,
              work.event.session_epoch, work.event.layout_version, 1, &event) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(host, HostToken, 101, &poll_state) ==
              InteropLinuxSocketSuccess);
        override_ack_event_phase = InteropPeerTransportClosed;
        Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
              101, POLLOUT, 0, InteropLinuxSocketMaxTxBudgetBytes,
              InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(override_ack_event_phase == -1);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.tx_bytes != 0 && work.terminal_reason ==
              InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
        Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    }
}

static void test_carry_and_empty_peer_buffer_fault_paths(void)
{
    {
        int pair[2];
        const uint64_t Token = 0x217;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        interop_peer_event_t event;
        interop_writer_t writer;
        uint8_t input[InteropLinuxSocketFrameCapacity];
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        size_t hello_length = make_hello_frame(input, sizeof(input), 1, 0x2170);
        size_t feature_length = 0;

        interop_writer_init(&writer, input + hello_length, sizeof(input) - hello_length);
        Check(interop_put_uint(&writer, InteropTagWindow, 3) == InteropErrorNone);
        Check(interop_put_uint(&writer, InteropTagGeneration, 4) == InteropErrorNone);
        Check(interop_writer_finish(&writer, InteropMsgWindowRetire, 2,
                                    InteropFromGuest, &feature_length) ==
              InteropErrorNone);
        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], input, hello_length + feature_length, MSG_NOSIGNAL) ==
              (ssize_t)(hello_length + feature_length));
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
              InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(work.event.kind == InteropPeerEventValidateMapping);
        Check(interop_linux_socket_mapping_result(owner, Token, 100,
              work.event.session_epoch, work.event.layout_version, 1, &event) ==
              InteropLinuxSocketSuccess);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_peer_call = PeerCallFeed;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(fail_peer_call == PeerCallNone);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }

    {
        int pair[2];
        const uint64_t Token = 0x218;
        interop_linux_socket_t *owner;
        interop_linux_socket_poll_t poll_state;
        interop_linux_socket_work_t work;
        uint8_t hello[InteropLinuxSocketFrameCapacity];
        uint8_t frame[InteropLinuxSocketFrameCapacity];
        size_t hello_length = make_hello_frame(hello, sizeof(hello), 1, 0x2180);

        Check(hello_length == 60);
        create_pair(pair);
        owner = create_owner(pair[0], InteropPeerHost, Token);
        Check(send(pair[1], hello, hello_length, MSG_NOSIGNAL) == (ssize_t)hello_length);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        recv_chunk_two_byte_calls = 29;
        recv_chunk_schedule_calls = 31;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, POLLIN, hello_length, 0, InteropLinuxSocketMaxSyscallBudget,
              frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
        Check(work.rx_bytes == hello_length && work.event.kind == InteropPeerEventNone);
        Check(work.internal_steps == InteropLinuxSocketMaxInternalStepsPerDrive - 1u);
        Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
              InteropLinuxSocketSuccess);
        fail_peer_call = PeerCallPendingBytes;
        Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
              100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
              InteropLinuxSocketSuccess);
        Check(fail_peer_call == PeerCallNone);
        Check(work.status == InteropLinuxSocketWorkPeerFailure);
        Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
        Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
        Check(close(pair[1]) == 0);
    }
}

static void test_mapping_wait_carry_full_and_hangup(void)
{
    int pair[2];
    const uint64_t Token = 0x219;
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t payload[InteropLinuxSocketRxCarryBytes + 512];
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    memset(payload, 0x5A, sizeof(payload));
    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    Check(send(pair[1], payload, sizeof(payload), MSG_NOSIGNAL) ==
          (ssize_t)sizeof(payload));
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    recv_chunk_limit = 512;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0, 1,
          frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.rx_bytes == 512 && work.state == InteropLinuxSocketMappingWait);

    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.rx_bytes == InteropLinuxSocketRxCarryBytes - 512);
    Check(work.state == InteropLinuxSocketMappingWait);
    Check(work.syscalls > 1);
    Check(close(pair[1]) == 0);
    Check(poll_drive(owner, Token, 100, InteropLinuxSocketMaxRxBudgetBytes,
          0, &work) == InteropLinuxSocketSuccess);
    Check(work.rx_bytes == 0 && work.syscalls == 0);
    Check(work.state == InteropLinuxSocketMappingWait);
    recv_chunk_limit = 0;
    Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
}

static void test_drain_deadline_saturates_near_uint64_max(void)
{
    int pair[2];
    interop_linux_socket_config_t config;
    interop_linux_socket_t *owner = NULL;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    const uint64_t StartMs = UINT64_MAX - UINT64_C(100);
    const uint64_t Token = 0x209;

    create_pair(pair);
    config = make_config(InteropPeerHost, Token);
    config.peer_config.accepted_at_ms = StartMs;
    config.drain_timeout_ms = 5000;
    Check(interop_linux_socket_create(pair[0], &config, &owner) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_begin_drain(owner, Token, StartMs, &event) ==
          InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining);
    Check(interop_linux_socket_poll_events(owner, Token, UINT64_MAX - 1,
          &poll_state) == InteropLinuxSocketSuccess);
    Check(poll_state.timeout_ms == 1 && poll_state.immediate_work == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          UINT64_MAX - 1, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketDraining);
    Check(interop_linux_socket_poll_events(owner, Token, UINT64_MAX,
          &poll_state) == InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1 && poll_state.timeout_ms == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          UINT64_MAX, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainDeadline);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_socketpair_handshake_fragmented(void)
{
    int pair[2];
    const uint64_t HostToken = 0x301;
    const uint64_t GuestToken = 0x302;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t guest_map_event;
    interop_peer_event_t host_map_event;
    interop_peer_event_t host_ready_event;
    interop_linux_socket_work_t work;
    uint8_t inbound_frames[3 * 64];
    size_t inbound_length = 0;
    size_t frame_length;
    uint64_t now_ms = 100;
    unsigned index;

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, now_ms, 0x4455,
          InteropLayoutVersion, 1, &guest_map_event) == InteropLinuxSocketSuccess);

    memset(&host_map_event, 0, sizeof(host_map_event));
    for (index = 0; index < 256 && host_map_event.kind != InteropPeerEventValidateMapping; ++index) {
        ++now_ms;
        Check(poll_drive(guest, GuestToken, now_ms, 0, 1, &work) == InteropLinuxSocketSuccess);
        Check(work.tx_bytes <= 1);
        Check(poll_drive(host, HostToken, now_ms, 1, 0, &work) == InteropLinuxSocketSuccess);
        if (work.event.kind != InteropPeerEventNone)
            host_map_event = work.event;
    }
    Check(host_map_event.kind == InteropPeerEventValidateMapping);
    Check(host_map_event.session_epoch == 0x4455);
    Check(interop_linux_socket_mapping_result(host, HostToken, now_ms,
          host_map_event.session_epoch, host_map_event.layout_version, 1,
          &host_ready_event) == InteropLinuxSocketSuccess);

    memset(&work, 0, sizeof(work));
    for (index = 0; index < 256 && work.event.kind != InteropPeerEventReady; ++index) {
        ++now_ms;
        Check(poll_drive(host, HostToken, now_ms, 0, 1, &work) == InteropLinuxSocketSuccess);
        Check(work.tx_bytes <= 1);
        Check(poll_drive(guest, GuestToken, now_ms, 1, 0, &work) == InteropLinuxSocketSuccess);
    }
    Check(work.event.kind == InteropPeerEventReady);
    Check(work.event.session_epoch == 0x4455);

    /* A valid host Ping is answered by the Guest owner's copied Pong queue. */
    frame_length = make_empty_frame(inbound_frames, sizeof(inbound_frames),
                                    InteropMsgPing, 73, InteropFromHost);
    Check(__real_send(pair[0], inbound_frames, frame_length, MSG_DONTWAIT) ==
          (ssize_t)frame_length);
    ++now_ms;
    Check(poll_drive(guest, GuestToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);
    ++now_ms;
    Check(poll_drive(guest, GuestToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);

    /* Host cadence is driven by poll/drive; the next calls exchange its Ping. */
    now_ms += 2000;
    Check(poll_drive(host, HostToken, now_ms, 0, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketActive);
    Check(poll_drive(host, HostToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);
    Check(poll_drive(guest, GuestToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);
    Check(poll_drive(guest, GuestToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);
    Check(poll_drive(host, HostToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);

    /* Exercise three distinct Ready dispatches from real coalesced socket data. */
    {
        interop_writer_t writer;
        size_t feature_length = 0;

        interop_writer_init(&writer, inbound_frames, sizeof(inbound_frames));
        Check(interop_put_uint(&writer, InteropTagWindow, 0x11223344) == InteropErrorNone);
        Check(interop_put_uint(&writer, InteropTagGeneration, 7) == InteropErrorNone);
        Check(interop_writer_finish(&writer, InteropMsgWindowRetire, 41,
                                   InteropFromGuest, &feature_length) == InteropErrorNone);
        inbound_length = feature_length;
    }
    frame_length = make_uint_frame(inbound_frames + inbound_length,
                                   sizeof(inbound_frames) - inbound_length,
                                   InteropMsgError, 42, InteropFromGuest,
                                   InteropTagErrorCode, InteropErrorProtocolViolation);
    inbound_length += frame_length;
    frame_length = make_uint_frame(inbound_frames + inbound_length,
                                   sizeof(inbound_frames) - inbound_length,
                                   InteropMsgGoodbye, 0, InteropFromGuest,
                                   InteropTagStatus, InteropCancelled);
    inbound_length += frame_length;
    Check(__real_send(pair[1], inbound_frames, inbound_length, MSG_DONTWAIT) ==
          (ssize_t)inbound_length);

    memset(&work, 0, sizeof(work));
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, sizeof(inbound_frames), 0, &work) ==
          InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventFeatureFrame);
    Check(work.event.message_type == InteropMsgWindowRetire);
    Check(work.event.request_id == 41 && work.event.copied_bytes != 0);

    memset(&work, 0, sizeof(work));
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 0, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventError);
    Check(work.event.message_type == InteropMsgError);
    Check(work.event.request_id == 42);

    memset(&work, 0, sizeof(work));
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 0, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventDraining);
    Check(work.event.message_type == InteropMsgGoodbye);
    Check(work.state == InteropLinuxSocketTransportClosed ||
          work.state == InteropLinuxSocketDraining);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_repeated_socketpair_session_lifecycles(void)
{
    unsigned index;

    /* Repeat the full fragmented negotiation, Ping/Pong and feature dispatch
     * over fresh real socketpairs to exercise teardown/reinitialization. */
    for (index = 0; index < 128; ++index)
        test_socketpair_handshake_fragmented();
}

static void test_ack_failure_counts_positive_send_and_closes(void)
{
    int pair[2];
    const uint64_t Token = 0x404;
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 77,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) == InteropLinuxSocketSuccess);
    fail_next_ack = 1;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLOUT, 0, 1, 4, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(fail_next_ack == 0);
    Check(work.status == InteropLinuxSocketWorkPeerFailure);
    Check(work.tx_bytes == 1);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerFailure);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_eof_and_single_close_diagnostic(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    interop_linux_socket_poll_t poll_state;
    const uint64_t Token = 0x505;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    Check(close(pair[1]) == 0);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN | POLLHUP, 16, 0, 4, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalEof);
    Check(interop_linux_socket_get_closed_snapshot(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(work.terminal_reason == InteropLinuxSocketTerminalEof);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, 0x507);
    Check(close(pair[1]) == 0);
    Check(interop_linux_socket_poll_events(owner, 0x507, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    fail_transport_lost_count = 1;
    Check(interop_linux_socket_drive(owner, 0x507, poll_state.poll_cookie,
          100, POLLIN | POLLHUP, 16, 0, 4, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.status == InteropLinuxSocketWorkPeerFailure);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalEof);
    Check(interop_linux_socket_get_closed_snapshot(owner, 0x507, &work) ==
          InteropLinuxSocketSuccess);
    Check(work.status == InteropLinuxSocketWorkTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalEof);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, 0x506);
    fail_next_close = 1;
    wrapped_close_calls = 0;
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketCloseDiagnostic);
    Check(owner == NULL && wrapped_close_calls == 1 && errno == EINTR);
    Check(close(pair[1]) == 0);
}

static void test_socket_error_preserves_first_errno_over_close_diagnostic(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    const uint64_t Token = 0x199;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    fail_next_recv_errno = EIO;
    fail_next_close = 1;
    wrapped_close_calls = 0;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN, 4, 0, 4, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalSocketError);
    Check(work.system_errno == EIO && wrapped_close_calls == 1);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketCloseDiagnostic);
    Check(owner == NULL && errno == EINTR && wrapped_close_calls == 1);
    Check(close(pair[1]) == 0);
}

static void test_so_error_eintr_defers_dispatch_until_retry(void)
{
    int pair[2];
    const uint64_t HostToken = 0x508;
    const uint64_t GuestToken = 0x509;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t host_poll;
    interop_linux_socket_poll_t guest_poll;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x5080,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &guest_poll) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, GuestToken, guest_poll.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketFrameCapacity,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &host_poll) ==
          InteropLinuxSocketSuccess);
    fail_next_so_error = 1;
    Check(interop_linux_socket_drive(host, HostToken, host_poll.poll_cookie,
          100, POLLIN | POLLERR, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(fail_next_so_error == 0);
    Check(work.rx_bytes == 0 && work.event.kind == InteropPeerEventNone);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &host_poll) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, host_poll.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.rx_bytes != 0);
    Check(work.event.kind == InteropPeerEventValidateMapping);

    Check(interop_linux_socket_abort(host, HostToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_zero_so_error_allows_buffered_hello_dispatch(void)
{
    int pair[2];
    const uint64_t HostToken = 0x50E;
    const uint64_t GuestToken = 0x50F;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t host_poll;
    interop_linux_socket_poll_t guest_poll;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x50E0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &guest_poll) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, GuestToken, guest_poll.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketFrameCapacity,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);
    Check(interop_linux_socket_poll_events(host, HostToken, 100, &host_poll) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, host_poll.poll_cookie,
          100, POLLIN | POLLERR, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventValidateMapping);
    Check(work.state == InteropLinuxSocketMappingWait);
    Check(interop_linux_socket_abort(host, HostToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_header_error_aborts_with_handshake_tx_pending(void)
{
    int pair[2];
    const uint64_t Token = 0x510;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t bad_header[InteropHeaderBytes] = {0};
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    guest = create_owner(pair[0], InteropPeerGuest, Token);
    Check(interop_linux_socket_mapping_result(guest, Token, 100, 0x5100,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(send(pair[1], bad_header, sizeof(bad_header), MSG_NOSIGNAL) ==
          (ssize_t)sizeof(bad_header));
    peer_next_event_calls = 0;
    Check(interop_linux_socket_poll_events(guest, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, Token, poll_state.poll_cookie,
          100, POLLIN, sizeof(bad_header), 0, 4, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.status == InteropLinuxSocketWorkTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerAbort);
    Check(work.diagnostic_event.kind == InteropPeerEventDraining);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_next_event_abort_preserves_terminal_work_status(void)
{
    int pair[2];
    const uint64_t Token = 0x20C;
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t premature_feature[InteropLinuxSocketFrameCapacity];
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    size_t premature_feature_length = 0;
    interop_writer_t writer;

    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerHost, Token);
    interop_writer_init(&writer, premature_feature, sizeof(premature_feature));
    Check(interop_put_uint(&writer, InteropTagWindow, 5) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagGeneration, 1) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgWindowRetire, 1,
                                InteropFromGuest, &premature_feature_length) ==
          InteropErrorNone);
    Check(send(pair[1], premature_feature, premature_feature_length, MSG_NOSIGNAL) ==
          (ssize_t)premature_feature_length);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    peer_next_event_calls = 0;
    force_abort_next_event = 1;
    fail_transport_lost_count = 1;
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(peer_next_event_calls != 0);
    Check(force_abort_next_event == 0);
    Check(fail_transport_lost_count == 0);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.status == InteropLinuxSocketWorkPeerFailure);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerAbort);
    Check(work.diagnostic_event.kind == InteropPeerEventDraining);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_begin_drain_aborts_with_unsent_hello(void)
{
    int pair[2];
    const uint64_t Token = 0x517;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;

    create_pair(pair);
    guest = create_owner(pair[0], InteropPeerGuest, Token);
    Check(interop_linux_socket_mapping_result(guest, Token, 100, 0x5170,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_begin_drain(guest, Token, 100, &event) ==
          InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining);
    Check((event.flags & InteropPeerAbortTransport) != 0);
    Check(interop_linux_socket_get_closed_snapshot(guest, Token, &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalPeerAbort);
    Check(interop_linux_socket_poll_events(guest, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.fd == -1);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_handshake_timeout_draining_event_is_not_repeated(void)
{
    int pair[2];
    const uint64_t Token = 0x518;
    const uint64_t TimeoutAt = 100 + InteropHelloTimeoutMs;
    interop_linux_socket_t *host;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, Token);
    Check(interop_linux_socket_poll_events(host, Token, TimeoutAt, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, Token, poll_state.poll_cookie,
          TimeoutAt, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventDraining);
    Check(work.state == InteropLinuxSocketDraining && work.tx_bytes == 0);
    Check(interop_linux_socket_poll_events(host, Token, TimeoutAt, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1);
    Check(interop_linux_socket_drive(host, Token, poll_state.poll_cookie,
          TimeoutAt, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventNone);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainComplete);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_rx_steps_reserve_peer_ack_before_positive_send(void)
{
    int pair[2];
    const uint64_t Token = 0x519;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t incoming[InteropLinuxSocketFrameCapacity];
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    size_t incoming_length;

    create_pair(pair);
    guest = create_owner(pair[0], InteropPeerGuest, Token);
    Check(interop_linux_socket_mapping_result(guest, Token, 100, 0x5190,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    incoming_length = make_hello_ack_frame(incoming, sizeof(incoming), 1);
    Check(incoming_length > 29);
    Check(__real_send(pair[1], incoming, incoming_length, MSG_DONTWAIT) ==
          (ssize_t)incoming_length);
    Check(interop_linux_socket_poll_events(guest, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0);
    recv_chunk_schedule_calls = 29;
    recv_chunk_two_byte_calls = 0;
    Check(interop_linux_socket_drive(guest, Token, poll_state.poll_cookie,
          100, POLLIN | POLLOUT, 29, InteropLinuxSocketFrameCapacity, 64,
          frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(recv_chunk_schedule_calls == 0);
    Check(work.rx_bytes == 29 && work.tx_bytes == 0);
    Check(work.internal_steps == 62);
    Check(work.state == InteropLinuxSocketActive && work.event.kind == InteropPeerEventNone);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_host_mapping_callback_at_handshake_deadline_drains(void)
{
    int pair[2];
    const uint64_t HostToken = 0x51A;
    const uint64_t GuestToken = 0x51B;
    const uint64_t Deadline = 100 + InteropHelloTimeoutMs;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x51A0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(poll_drive(guest, GuestToken, 101, 0, 64, &work) == InteropLinuxSocketSuccess);
    Check(poll_drive(host, HostToken, 101, 64, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventValidateMapping);
    Check(interop_linux_socket_mapping_result(host, HostToken, Deadline,
          work.event.session_epoch, work.event.layout_version, 1, &event) ==
          InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining);
    Check(event.phase == InteropPeerDraining);

    Check(interop_linux_socket_poll_events(host, HostToken, Deadline + 500,
          &poll_state) == InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          Deadline + 500, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainDeadline);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
}

static void test_first_liveness_draining_event_precedes_queued_tx(void)
{
    int pair[2];
    const uint64_t HostToken = 0x511;
    const uint64_t GuestToken = 0x512;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t guest_map_event;
    interop_peer_event_t host_map_event;
    interop_peer_event_t host_ready_event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    uint64_t now_ms = 100;
    unsigned index;

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, now_ms, 0x5110,
          InteropLayoutVersion, 1, &guest_map_event) == InteropLinuxSocketSuccess);
    memset(&host_map_event, 0, sizeof(host_map_event));
    for (index = 0; index < 256 && host_map_event.kind != InteropPeerEventValidateMapping; ++index) {
        ++now_ms;
        Check(poll_drive(guest, GuestToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(host, HostToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
        if (work.event.kind != InteropPeerEventNone)
            host_map_event = work.event;
    }
    Check(host_map_event.kind == InteropPeerEventValidateMapping);
    Check(interop_linux_socket_mapping_result(host, HostToken, now_ms,
          host_map_event.session_epoch, host_map_event.layout_version, 1,
          &host_ready_event) == InteropLinuxSocketSuccess);
    memset(&work, 0, sizeof(work));
    for (index = 0; index < 256 && work.event.kind != InteropPeerEventReady; ++index) {
        ++now_ms;
        Check(poll_drive(host, HostToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(guest, GuestToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    }
    Check(work.event.kind == InteropPeerEventReady);

    /* Tick enqueues a host Ping at 2s; leave it queued until liveness expiry. */
    now_ms += 2000;
    Check(interop_linux_socket_poll_events(host, HostToken, now_ms, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          now_ms, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketActive && work.tx_bytes == 0);
    Check(interop_linux_socket_poll_events(host, HostToken, now_ms, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0);

    now_ms += 4000;
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          now_ms, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventDraining);
    Check(work.state == InteropLinuxSocketDraining && work.tx_bytes == 0);
    Check(work.terminal_reason == InteropLinuxSocketTerminalNone);

    ++now_ms;
    Check(interop_linux_socket_poll_events(host, HostToken, now_ms, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          now_ms, POLLOUT, 0, 64, 8, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketDraining && work.tx_bytes != 0);
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 0, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainComplete);

    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
}

static void test_hup_surfaces_complete_frame_before_eof(void)
{
    int pair[2];
    const uint64_t HostToken = 0x513;
    const uint64_t GuestToken = 0x514;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x5130,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, GuestToken, poll_state.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketFrameCapacity,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);
    Check(shutdown(pair[1], SHUT_WR) == 0);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, POLLIN | POLLHUP, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventValidateMapping);
    Check(work.state == InteropLinuxSocketMappingWait);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, POLLIN | POLLHUP, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalEof);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_repeated_hello_in_ready_drains_without_auto_replay(void)
{
    int pair[2];
    const uint64_t HostToken = 0x515;
    const uint64_t GuestToken = 0x516;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t host_map_event;
    interop_peer_event_t guest_event;
    interop_linux_socket_work_t work;
    uint8_t wire[InteropLinuxSocketFrameCapacity];
    size_t wire_length;
    uint64_t now_ms = 100;
    unsigned index;

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, now_ms, 0x5150,
          InteropLayoutVersion, 1, &guest_event) == InteropLinuxSocketSuccess);
    memset(&host_map_event, 0, sizeof(host_map_event));
    for (index = 0; index < 256 && host_map_event.kind != InteropPeerEventValidateMapping; ++index) {
        ++now_ms;
        Check(poll_drive(guest, GuestToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(host, HostToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
        if (work.event.kind != InteropPeerEventNone)
            host_map_event = work.event;
    }
    Check(host_map_event.kind == InteropPeerEventValidateMapping);
    Check(interop_linux_socket_mapping_result(host, HostToken, now_ms,
          host_map_event.session_epoch, host_map_event.layout_version, 1,
          &guest_event) == InteropLinuxSocketSuccess);
    memset(&work, 0, sizeof(work));
    for (index = 0; index < 256 && work.event.kind != InteropPeerEventReady; ++index) {
        ++now_ms;
        Check(poll_drive(host, HostToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive(guest, GuestToken, now_ms, 64, 0, &work) == InteropLinuxSocketSuccess);
    }
    Check(work.event.kind == InteropPeerEventReady);

    wire_length = make_hello_frame(wire, sizeof(wire), 88, 0x9911);
    Check(__real_send(pair[1], wire, wire_length, MSG_DONTWAIT) == (ssize_t)wire_length);
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, InteropLinuxSocketMaxRxBudgetBytes, 0,
                     &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventDraining);
    Check(work.state == InteropLinuxSocketDraining && work.tx_bytes == 0);

    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 0, 64, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketDraining && work.tx_bytes != 0);
    Check(work.event.kind == InteropPeerEventNone);
    ++now_ms;
    Check(poll_drive(host, HostToken, now_ms, 0, 0, &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketTransportClosed);
    Check(work.terminal_reason == InteropLinuxSocketTerminalDrainComplete);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_full_send_buffer_keeps_pollout_guidance(void)
{
    int pair[2];
    uint8_t filler[512];
    uint8_t discard[512];
    size_t filler_bytes = 0;
    const uint64_t Token = 0x50E;
    interop_linux_socket_t *owner;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    memset(filler, 0xA7, sizeof(filler));
    create_pair(pair);
    for (;;) {
        ssize_t sent = send(pair[0], filler, sizeof(filler), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            filler_bytes += (size_t)sent;
            continue;
        }
        Check(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    Check(filler_bytes != 0);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    Check(interop_linux_socket_mapping_result(owner, Token, 100, 0x50E0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0 && poll_state.immediate_work == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketMaxTxBudgetBytes, 8,
          frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.tx_bytes == 0 && work.syscalls != 0 &&
          work.state == InteropLinuxSocketActive);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0 && poll_state.immediate_work == 0);

    while (filler_bytes != 0) {
        ssize_t received = recv(pair[1], discard, sizeof(discard), MSG_DONTWAIT);
        if (received > 0) {
            size_t received_bytes = (size_t)received;
            Check(received_bytes <= filler_bytes);
            filler_bytes -= received_bytes;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        Check(received == 0 || (received < 0 &&
              (errno == EAGAIN || errno == EWOULDBLOCK)));
        break;
    }
    Check(filler_bytes == 0);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLOUT) != 0 && poll_state.immediate_work == 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketMaxTxBudgetBytes,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes == 60 && work.state == InteropLinuxSocketActive);
    Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_mapping_wait_fills_carry_without_read_spin(void)
{
    int pair[2];
    interop_linux_socket_t *owner;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t bytes[InteropLinuxSocketRxCarryBytes];
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    const uint64_t Token = 0x50F;

    memset(bytes, 0x7B, sizeof(bytes));
    create_pair(pair);
    owner = create_owner(pair[0], InteropPeerGuest, Token);
    Check(send(pair[1], bytes, sizeof(bytes), MSG_NOSIGNAL) == (ssize_t)sizeof(bytes));
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLIN) != 0);
    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketRxCarryBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.rx_bytes == sizeof(bytes) && work.state == InteropLinuxSocketMappingWait);
    Check(interop_linux_socket_poll_events(owner, Token, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check((poll_state.events & POLLIN) == 0 && poll_state.immediate_work == 0);

    Check(interop_linux_socket_drive(owner, Token, poll_state.poll_cookie,
          100, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.state == InteropLinuxSocketMappingWait && work.rx_bytes == 0);
    Check(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    Check(work.status == InteropLinuxSocketWorkTransportClosed);
    Check(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    Check(close(pair[1]) == 0);
}

static void test_empty_carry_rechecks_completed_peer_frame(void)
{
    int pair[2];
    const uint64_t HostToken = 0x50A;
    const uint64_t GuestToken = 0x50B;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x50A0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, GuestToken, poll_state.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketFrameCapacity,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes != 0);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    arm_pending_suppression_after_recv = 1;
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(arm_pending_suppression_after_recv == 0 && suppress_pending_after_recv == 0);
    Check(work.rx_bytes != 0 && work.event.kind == InteropPeerEventNone);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1 && poll_state.timeout_ms == 0);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, 0, 0, 0, 0, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventValidateMapping);

    Check(interop_linux_socket_abort(host, HostToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}

static void test_step_boundary_rechecks_completed_peer_frame(void)
{
    int pair[2];
    const uint64_t HostToken = 0x50C;
    const uint64_t GuestToken = 0x50D;
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t event;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    create_pair(pair);
    host = create_owner(pair[0], InteropPeerHost, HostToken);
    guest = create_owner(pair[1], InteropPeerGuest, GuestToken);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, 100, 0x50C0,
          InteropLayoutVersion, 1, &event) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_poll_events(guest, GuestToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(guest, GuestToken, poll_state.poll_cookie,
          100, POLLOUT, 0, InteropLinuxSocketFrameCapacity,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.tx_bytes == 60);

    recv_chunk_two_byte_calls = 29;
    recv_chunk_schedule_calls = 31;
    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, POLLIN, InteropLinuxSocketMaxRxBudgetBytes, 0,
          InteropLinuxSocketMaxSyscallBudget, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(recv_chunk_two_byte_calls == 0 && recv_chunk_schedule_calls == 0);
    Check(work.rx_bytes == 60 && work.internal_steps ==
          InteropLinuxSocketMaxInternalStepsPerDrive - 1u);
    Check(work.event.kind == InteropPeerEventNone);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 1 && poll_state.timeout_ms == 0);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie,
          100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
          InteropLinuxSocketSuccess);
    Check(work.event.kind == InteropPeerEventValidateMapping);

    Check(interop_linux_socket_poll_events(host, HostToken, 100, &poll_state) ==
          InteropLinuxSocketSuccess);
    Check(poll_state.immediate_work == 0);
    Check(interop_linux_socket_abort(host, HostToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_abort(guest, GuestToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&host) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_destroy(&guest) == InteropLinuxSocketSuccess);
}


/* Connection tokens shared by the feature pass-through helpers below. */
static const uint64_t HostToken = 0x301;
static const uint64_t GuestToken = 0x302;

/** @brief Build a complete Ready-capable config with an explicit facility offer. */
static interop_linux_socket_config_t make_config_caps(uint32_t role, uint64_t token, uint64_t caps)
{
    interop_linux_socket_config_t config = make_config(role, token);

    config.peer_config.local_capabilities = caps;
    return config;
}

static interop_linux_socket_t *create_owner_caps(int fd, uint32_t role, uint64_t token, uint64_t caps)
{
    interop_linux_socket_config_t config = make_config_caps(role, token, caps);
    interop_linux_socket_t *owner = NULL;

    Check(interop_linux_socket_create(fd, &config, &owner) == InteropLinuxSocketSuccess);
    Check(owner != NULL);
    return owner;
}

/** @brief Drive one poll snapshot and copy any admitted frame into the caller's buffer. */
static int poll_drive_into(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
                           size_t rx_budget, size_t tx_budget, uint8_t *frame,
                           interop_linux_socket_work_t *work)
{
    interop_linux_socket_poll_t poll_state;
    struct pollfd descriptor;
    int poll_result;

    Check(interop_linux_socket_poll_events(owner, token, now_ms, &poll_state) == InteropLinuxSocketSuccess);
    descriptor.fd = poll_state.fd;
    descriptor.events = (short)poll_state.events;
    descriptor.revents = 0;
    poll_result = poll(&descriptor, 1, 0);
    Check(poll_result >= 0);
    return interop_linux_socket_drive(owner, token, poll_state.poll_cookie, now_ms,
                                      (uint32_t)(unsigned short)descriptor.revents,
                                      rx_budget, tx_budget, 16, frame,
                                      InteropLinuxSocketFrameCapacity, work);
}

/** @brief Complete the handshake over a fresh socketpair using the same bounded steps as the fragmented test. */
static void make_ready_pair(uint64_t caps, int pair[2], interop_linux_socket_t **host_out,
                            interop_linux_socket_t **guest_out, uint64_t *now_out)
{
    interop_linux_socket_t *host;
    interop_linux_socket_t *guest;
    interop_peer_event_t guest_map_event;
    interop_peer_event_t host_map_event;
    interop_peer_event_t host_ready_event;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    uint64_t now_ms = 100;
    unsigned index;

    create_pair(pair);
    host = create_owner_caps(pair[0], InteropPeerHost, HostToken, caps);
    guest = create_owner_caps(pair[1], InteropPeerGuest, GuestToken, caps);
    Check(interop_linux_socket_mapping_result(guest, GuestToken, now_ms, 0x4455,
          InteropLayoutVersion, 1, &guest_map_event) == InteropLinuxSocketSuccess);
    memset(&host_map_event, 0, sizeof(host_map_event));
    for (index = 0; index < 256 && host_map_event.kind != InteropPeerEventValidateMapping; ++index) {
        ++now_ms;
        Check(poll_drive_into(guest, GuestToken, now_ms, 0, 1, frame, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive_into(host, HostToken, now_ms, 1, 0, frame, &work) == InteropLinuxSocketSuccess);
        if (work.event.kind != InteropPeerEventNone)
            host_map_event = work.event;
    }
    Check(host_map_event.kind == InteropPeerEventValidateMapping);
    Check(interop_linux_socket_mapping_result(host, HostToken, now_ms,
          host_map_event.session_epoch, host_map_event.layout_version, 1,
          &host_ready_event) == InteropLinuxSocketSuccess);
    memset(&work, 0, sizeof(work));
    for (index = 0; index < 256 && work.event.kind != InteropPeerEventReady; ++index) {
        ++now_ms;
        Check(poll_drive_into(host, HostToken, now_ms, 0, 1, frame, &work) == InteropLinuxSocketSuccess);
        Check(poll_drive_into(guest, GuestToken, now_ms, 1, 0, frame, &work) == InteropLinuxSocketSuccess);
    }
    Check(work.event.kind == InteropPeerEventReady);
    *host_out = host;
    *guest_out = guest;
    *now_out = now_ms;
}

/** @brief Flush sender bytes and receive until the receiver reports one feature frame. */
static void deliver_feature(interop_linux_socket_t *sender, uint64_t sender_token,
                            interop_linux_socket_t *receiver, uint64_t receiver_token,
                            uint64_t *now_ms, uint8_t *frame, interop_linux_socket_work_t *received)
{
    interop_linux_socket_work_t sender_work;
    unsigned index;

    memset(received, 0, sizeof(*received));
    for (index = 0; index < 64 && received->event.kind != InteropPeerEventFeatureFrame; ++index) {
        ++*now_ms;
        Check(poll_drive_into(sender, sender_token, *now_ms, 0, 64, frame, &sender_work) == InteropLinuxSocketSuccess);
        Check(poll_drive_into(receiver, receiver_token, *now_ms, 64, 0, frame, received) == InteropLinuxSocketSuccess);
    }
    Check(received->event.kind == InteropPeerEventFeatureFrame);
}

/** @brief Encode a frame with two unsigned fields, optionally carrying a request identifier. */
static size_t make_two_uint_frame(uint8_t *frame, size_t capacity, uint16_t type,
                                  uint32_t request_id, interop_direction_t direction,
                                  uint16_t tag_a, uint64_t value_a, uint16_t tag_b, uint64_t value_b)
{
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, tag_a, value_a) == InteropErrorNone);
    Check(interop_put_uint(&writer, tag_b, value_b) == InteropErrorNone);
    Check(interop_writer_finish(&writer, type, request_id, direction, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode a KeyboardLayout frame, a facility this connection never negotiates. */
static size_t make_layout_frame(uint8_t *frame, size_t capacity)
{
    static const uint8_t Layout[] = "00000409";
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_bytes(&writer, InteropTagLayout, Layout, sizeof(Layout) - 1) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgKeyboardLayout, 0, InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode a LaunchRequest; AppKey precedes LaunchKind to keep tags increasing. */
static size_t make_launch_request_frame(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    static const uint8_t AppKey[] = "a-Microsoft_Windows_Explorer";
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_bytes(&writer, InteropTagAppKey, AppKey, sizeof(AppKey) - 1) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagLaunchKind, InteropLaunchApp) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgLaunchRequest, request_id, InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Encode a LaunchResult; ProcessId precedes Status to keep tags increasing. */
static size_t make_launch_result_frame(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    Check(interop_put_uint(&writer, InteropTagProcessId, 4242) == InteropErrorNone);
    Check(interop_put_uint(&writer, InteropTagStatus, InteropOk) == InteropErrorNone);
    Check(interop_writer_finish(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

static void test_feature_pass_through_admission(void)
{
    static uint8_t frame[InteropLinuxSocketFrameCapacity];
    static uint8_t received_frame[InteropLinuxSocketFrameCapacity];
    const uint64_t Caps = InteropCapWindows | InteropCapInput | InteropCapLaunch;
    interop_linux_socket_work_t work;
    interop_linux_socket_poll_t poll_state;
    interop_linux_socket_t *host = NULL;
    interop_linux_socket_t *guest = NULL;
    interop_peer_event_t event;
    uint64_t now;
    uint32_t request_id = 0xDEAD;
    int pair[2];
    size_t length;

    make_ready_pair(Caps, pair, &host, &guest, &now);
    length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                 InteropTagWindow, 0x55, InteropTagGeneration, 2);
    Check(interop_linux_socket_enqueue_feature(NULL, HostToken, now + 1, frame, length, &event) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length, NULL) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length,
          (interop_peer_event_t *)(void *)frame) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, NULL, length, &event) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, 0, &event) == InteropLinuxSocketInvalidFrame);
    Check(interop_linux_socket_enqueue_feature(host, GuestToken, now + 1, frame, length, &event) == InteropLinuxSocketWrongToken);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now - 1, frame, length, &event) == InteropLinuxSocketTimeWentBackwards);

    /* Output and input ranges must be aligned, disjoint from the owner and disjoint from each other. */
    {
        static uint8_t misaligned_event[sizeof(interop_peer_event_t) + 16];
        static uint8_t misaligned_id[sizeof(uint32_t) + 16];
        uint8_t *storage = (uint8_t *)host;
        length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                     InteropTagWindow, 0x55, InteropTagGeneration, 2);
        Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length,
              (interop_peer_event_t *)(void *)(misaligned_event + 1)) == InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length,
              (interop_peer_event_t *)(void *)host) == InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, storage, 16, &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, 0, (uint32_t *)(void *)(misaligned_id + 1), &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, 0, (uint32_t *)(void *)host, &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, 0, &request_id,
              (interop_peer_event_t *)(void *)&request_id) == InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, storage, 16, &request_id, &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, 16, (uint32_t *)(void *)(frame + 12), &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, 16, &request_id,
              (interop_peer_event_t *)(void *)(frame + 8)) == InteropLinuxSocketInvalidArgument);
        Check(request_id == 0xDEAD);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id,
              (interop_peer_event_t *)(void *)(misaligned_event + 1)) == InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id,
              (interop_peer_event_t *)(void *)host) == InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now - 1, frame, length, &request_id, &event) ==
              InteropLinuxSocketTimeWentBackwards);
        Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, (const uint8_t *)(UINTPTR_MAX - 4), 16, &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, (const uint8_t *)(UINTPTR_MAX - 4), 16, &request_id, &event) ==
              InteropLinuxSocketInvalidArgument);
        Check(request_id == 0xDEAD);
    }
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length, &event) == InteropLinuxSocketUnsupportedFrame);
    length = make_layout_frame(frame, sizeof(frame));
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length, &event) == InteropLinuxSocketFeatureNotNegotiated);
    length = make_launch_result_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_feature(guest, GuestToken, now + 1, frame, length, &event) == InteropLinuxSocketInvalidFrame);

    /* A successful admission invalidates the earlier snapshot and asks for POLLOUT next. */
    length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                 InteropTagWindow, 0x55, InteropTagGeneration, 2);
    Check(interop_linux_socket_poll_events(host, HostToken, now + 1, &poll_state) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 2, frame, length, &event) == InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventNone && event.phase == InteropPeerReady);
    Check(interop_linux_socket_drive(host, HostToken, poll_state.poll_cookie, now + 2, 0, 0, 0, 0,
          received_frame, sizeof(received_frame), &work) == InteropLinuxSocketStalePoll);
    Check(interop_linux_socket_poll_events(host, HostToken, now + 2, &poll_state) == InteropLinuxSocketSuccess);
    Check((poll_state.events & (uint32_t)POLLOUT) != 0);
    now = now + 2;
    deliver_feature(host, HostToken, guest, GuestToken, &now, received_frame, &work);
    Check(work.event.message_type == InteropMsgWindowClose && work.event.request_id == 0);
    Check(work.event.copied_bytes == length && memcmp(received_frame, frame, length) == 0);

    /* Request identifiers: refusals leave the caller's identifier untouched, then stamp, echo and release. */
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, NULL, &event) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id, NULL) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length,
          (uint32_t *)(void *)frame, &event) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_enqueue_request(host, GuestToken, now + 1, frame, length, &request_id, &event) == InteropLinuxSocketWrongToken);
    length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                 InteropTagWindow, 0x55, InteropTagGeneration, 2);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id, &event) == InteropLinuxSocketUnsupportedFrame);
    Check(request_id == 0xDEAD);
    length = make_launch_request_frame(frame, sizeof(frame), 5);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id, &event) == InteropLinuxSocketInvalidFrame);
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 2, frame, length, &request_id, &event) == InteropLinuxSocketSuccess);
    Check(request_id == 1 && event.kind == InteropPeerEventNone);
    now = now + 2;
    deliver_feature(host, HostToken, guest, GuestToken, &now, received_frame, &work);
    Check(work.event.message_type == InteropMsgLaunchRequest && work.event.request_id == 1);
    Check(interop_linux_socket_release_request(host, GuestToken, 1) == InteropLinuxSocketWrongToken);
    Check(interop_linux_socket_release_request(host, HostToken, 0) == InteropLinuxSocketInvalidArgument);
    Check(interop_linux_socket_release_request(host, HostToken, 1) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_release_request(host, HostToken, 1) == InteropLinuxSocketUnknownRequestId);
    Check(interop_linux_socket_release_request(NULL, HostToken, 1) == InteropLinuxSocketInvalidArgument);

    /* The guest echoes the identifier; the host receives the same value. */
    length = make_launch_result_frame(frame, sizeof(frame), 9);
    Check(interop_linux_socket_enqueue_feature(guest, GuestToken, now + 1, frame, length, &event) == InteropLinuxSocketSuccess);
    now = now + 1;
    deliver_feature(guest, GuestToken, host, HostToken, &now, received_frame, &work);
    Check(work.event.message_type == InteropMsgLaunchResult && work.event.request_id == 9);

    interop_linux_socket_destroy(&host);
    interop_linux_socket_destroy(&guest);
    close(pair[0]);
    close(pair[1]);
}

static void test_feature_pass_through_queue_and_drain(void)
{
    static uint8_t frame[InteropLinuxSocketFrameCapacity];
    const uint64_t Caps = InteropCapWindows | InteropCapInput | InteropCapLaunch;
    interop_linux_socket_work_t work;
    interop_linux_socket_t *host = NULL;
    interop_linux_socket_t *guest = NULL;
    interop_peer_event_t event;
    uint64_t now;
    uint32_t request_id = 0xDEAD;
    unsigned filled;
    int pair[2];
    size_t length;

    /* A full, unflushed Data class refuses both entry points and keeps the identifier cursor. */
    make_ready_pair(Caps, pair, &host, &guest, &now);
    for (filled = 0; filled < 504; ++filled) {
        length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                     InteropTagWindow, filled + 1, InteropTagGeneration, 1);
        Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length, &event) == InteropLinuxSocketSuccess);
    }
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id, &event) == InteropLinuxSocketQueueFull);
    Check(request_id == 0xDEAD);
    length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                 InteropTagWindow, 1, InteropTagGeneration, 1);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 1, frame, length, &event) == InteropLinuxSocketQueueFull);
    interop_linux_socket_destroy(&host);
    interop_linux_socket_destroy(&guest);
    close(pair[0]);
    close(pair[1]);

    /* Ready deadline expiry drains without queueing; further admissions are WrongPhase. */
    make_ready_pair(Caps, pair, &host, &guest, &now);
    length = make_two_uint_frame(frame, sizeof(frame), InteropMsgWindowClose, 0, InteropFromHost,
                                 InteropTagWindow, 7, InteropTagGeneration, 1);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 60000, frame, length, &event) == InteropLinuxSocketSuccess);
    Check(event.kind == InteropPeerEventDraining && event.phase == InteropPeerDraining);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 60001, frame, length, &event) == InteropLinuxSocketWrongPhase);
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 60001, frame, length, &request_id, &event) == InteropLinuxSocketWrongPhase);
    interop_linux_socket_destroy(&host);
    interop_linux_socket_destroy(&guest);
    close(pair[0]);
    close(pair[1]);

    /* Transport closure retires identifiers; admissions then report Closed. */
    make_ready_pair(Caps, pair, &host, &guest, &now);
    length = make_launch_request_frame(frame, sizeof(frame), 0);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 1, frame, length, &request_id, &event) == InteropLinuxSocketSuccess);
    Check(request_id == 1);
    Check(interop_linux_socket_abort(host, HostToken, &work) == InteropLinuxSocketSuccess);
    Check(interop_linux_socket_enqueue_feature(host, HostToken, now + 2, frame, length, &event) == InteropLinuxSocketClosed);
    Check(interop_linux_socket_enqueue_request(host, HostToken, now + 2, frame, length, &request_id, &event) == InteropLinuxSocketClosed);
    Check(interop_linux_socket_release_request(host, HostToken, 1) == InteropLinuxSocketRequestRetired);
    interop_linux_socket_destroy(&host);
    interop_linux_socket_destroy(&guest);
    close(pair[0]);
    close(pair[1]);
}

int main(void)
{
    test_descriptor_transfer_and_preflight();
    test_owner_allocation_failure_preserves_fd();
    test_descriptor_flag_probe_failures_preserve_fd();
    test_create_preflight_and_peer_result_mapping();
    test_drain_deadline_closes_at_equality();
    test_abort_tick_skips_fatal_socket_probe();
    test_pollnval_transport_notification_failure_is_reported();
    test_unexpected_peer_phases_fail_closed_at_all_observation_sites();
    test_invalid_feed_next_event_and_ack_phases_fail_closed();
    test_carry_and_empty_peer_buffer_fault_paths();
    test_mapping_wait_carry_full_and_hangup();
    test_drain_deadline_saturates_near_uint64_max();
    test_poll_and_tick_peer_faults_fail_closed();
    test_receive_faults_and_peer_event_fault_close();
    test_tx_faults_and_destroy_retries();
    test_copy_tx_success_with_invalid_length_fails_closed();
    test_draining_tx_query_failure_is_terminal();
    test_socket_syscall_fault_paths();
    test_guest_mapping_preflight_and_abort();
    test_non_drive_peer_invariants_fail_closed_and_preserve_output();
    test_host_mapping_failure_is_forwarded_and_deadline_is_fixed();
    test_socketpair_handshake_fragmented();
    test_repeated_socketpair_session_lifecycles();
    test_zero_so_error_allows_buffered_hello_dispatch();
    test_header_error_aborts_with_handshake_tx_pending();
    test_next_event_abort_preserves_terminal_work_status();
    test_begin_drain_aborts_with_unsent_hello();
    test_handshake_timeout_draining_event_is_not_repeated();
    test_rx_steps_reserve_peer_ack_before_positive_send();
    test_host_mapping_callback_at_handshake_deadline_drains();
    test_first_liveness_draining_event_precedes_queued_tx();
    test_hup_surfaces_complete_frame_before_eof();
    test_repeated_hello_in_ready_drains_without_auto_replay();
    test_ack_failure_counts_positive_send_and_closes();
    test_eof_and_single_close_diagnostic();
    test_socket_error_preserves_first_errno_over_close_diagnostic();
    test_so_error_eintr_defers_dispatch_until_retry();
    test_empty_carry_rechecks_completed_peer_frame();
    test_step_boundary_rechecks_completed_peer_frame();
    test_full_send_buffer_keeps_pollout_guidance();
    test_mapping_wait_fills_carry_without_read_spin();
    test_feature_pass_through_admission();
    test_feature_pass_through_queue_and_drain();
    interop_linux_socket_test_guards();
    test_linux_socket_owner_private();
    puts("linux_socket_owner: all tests passed");
    return 0;
}
