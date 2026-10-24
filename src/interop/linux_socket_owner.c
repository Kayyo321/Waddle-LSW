/**
 * @file linux_socket_owner.c
 * @brief Bounded pull-driven Linux stream transport owner for interop_peer_t.
 *
 * @ownership The owner exclusively owns its accepted fd and peer after create
 * succeeds. RX/TX arrays are embedded. No caller pointer or callback is saved.
 * @thread_safety Every operation is serialized on the pthread that created it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waddle/interop_linux_socket.h"
#include "waddle/interop_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { InternalWorkExhausted = -100 };

struct interop_linux_socket_t {
    int fd;
    pthread_t owner_thread;
    interop_peer_t *peer;
    uint64_t token;
    uint64_t last_ms;
    uint64_t drain_deadline_ms;
    uint64_t next_poll_cookie;
    uint64_t current_poll_cookie;
    uint32_t drain_timeout_ms;
    uint32_t role;
    uint32_t state;
    uint32_t terminal_reason;
    int32_t terminal_errno;
    int32_t close_errno;
    bool cookie_current;
    bool cookies_exhausted;
    bool drain_deadline_set;
    bool mapping_callback_pending;
    bool transport_notified;
    bool poll_error_pending;
    bool peer_notify_failed;
    bool closed;
    interop_peer_event_t terminal_event;
    uint8_t rx_carry[InteropLinuxSocketRxCarryBytes];
    size_t rx_start;
    size_t rx_length;
    uint8_t tx_copy[InteropLinuxSocketFrameCapacity];
    size_t tx_length;
    size_t tx_offset;
};

/** @brief Test whether a caller address range is nonwrapping and aligned. */
static bool range_valid(const void *pointer, size_t length, size_t alignment)
{
    uintptr_t address;

    if (pointer == NULL || alignment == 0)
        return false;
    address = (uintptr_t)pointer;
    if (address % alignment != 0)
        return false;
    if (length > (size_t)(UINTPTR_MAX - address))
        return false;
    return true;
}

/** @brief Return whether two previously validated nonempty ranges overlap. */
static bool ranges_overlap(const void *left, size_t left_length,
                           const void *right, size_t right_length)
{
    uintptr_t left_start = (uintptr_t)left;
    uintptr_t right_start = (uintptr_t)right;

    if (left_length == 0 || right_length == 0)
        return false;
    return left_start < right_start + right_length && right_start < left_start + left_length;
}

/** @brief Validate a caller output against the live owner allocation. */
static bool output_disjoint_from_owner(const interop_linux_socket_t *owner,
                                      const void *output, size_t output_length)
{
    return !ranges_overlap(owner, sizeof(*owner), output, output_length);
}

/** @brief Check owner-thread affinity without changing owner state. */
static bool on_owner_thread(const interop_linux_socket_t *owner)
{
    return pthread_equal(owner->owner_thread, pthread_self()) != 0;
}

/** @brief Match nonblocking readiness on platforms with aliased errno values. */
static bool would_block(int error_code)
{
#if EWOULDBLOCK == EAGAIN
    return error_code == EAGAIN;
#else
    return error_code == EAGAIN || error_code == EWOULDBLOCK;
#endif
}

/** @brief Check token before reading protocol state or caller time. */
static int check_owner_token(interop_linux_socket_t *owner, uint64_t token)
{
    if (!on_owner_thread(owner))
        return InteropLinuxSocketWrongThread;
    if (token == 0 || token != owner->token)
        return InteropLinuxSocketWrongToken;
    return InteropLinuxSocketSuccess;
}

/** @brief Check monotonic time without consuming it. */
static int check_time(const interop_linux_socket_t *owner, uint64_t now_ms)
{
    if (now_ms < owner->last_ms)
        return InteropLinuxSocketTimeWentBackwards;
    return InteropLinuxSocketSuccess;
}

/** @brief Commit a caller timestamp after successful preflight/operation. */
static void commit_time(interop_linux_socket_t *owner, uint64_t now_ms)
{
    owner->last_ms = now_ms;
}

/** @brief Map only peer-create's documented caller errors and allocation failure.
 * @note Every other result from create is an internal peer invariant, not a
 *       retryable operation status, so it is deliberately mapped to failure.
 */
static int map_peer_create_result(int result)
{
    switch (result) {
    case InteropPeerInvalidArgument:
        return InteropLinuxSocketInvalidArgument;
    case InteropPeerOutOfMemory:
        return InteropLinuxSocketOutOfMemory;
    default:
        return InteropLinuxSocketPeerFailure;
    }
}

/** @brief Zero all event bytes, including ABI padding. */
static void clear_event(interop_peer_event_t *event)
{
    memset(event, 0, sizeof(*event));
}

/** @brief Copy only defined peer event fields into zeroed ABI storage. */
static void copy_event(interop_peer_event_t *destination,
                       const interop_peer_event_t *source)
{
    clear_event(destination);
    destination->kind = source->kind;
    destination->phase = source->phase;
    destination->flags = source->flags;
    destination->error_code = source->error_code;
    destination->session_epoch = source->session_epoch;
    destination->capabilities = source->capabilities;
    destination->layout_version = source->layout_version;
    destination->message_type = source->message_type;
    destination->request_id = source->request_id;
    destination->reserved_zero = source->reserved_zero;
    destination->copied_bytes = source->copied_bytes;
}

/** @brief Saturate a finite addition without wrapping caller monotonic time. */
static uint64_t saturating_deadline(uint64_t now_ms, uint32_t timeout_ms)
{
    if ((uint64_t)timeout_ms > UINT64_MAX - now_ms)
        return UINT64_MAX;
    return now_ms + (uint64_t)timeout_ms;
}

/** @brief Start the finite drain timer once, never extending it. */
static void note_draining(interop_linux_socket_t *owner, uint64_t now_ms)
{
    owner->state = InteropLinuxSocketDraining;
    if (!owner->drain_deadline_set) {
        owner->drain_deadline_ms = saturating_deadline(now_ms, owner->drain_timeout_ms);
        owner->drain_deadline_set = true;
    }
}

/** @brief Save the first terminal peer event without replaying it later. */
static void save_terminal_event(interop_linux_socket_t *owner,
                                const interop_peer_event_t *event)
{
    if (owner->terminal_event.kind == InteropPeerEventNone &&
        event->kind == InteropPeerEventDraining)
        copy_event(&owner->terminal_event, event);
}

/** @brief Close an owned descriptor exactly once and report transport loss. */
static void close_transport(interop_linux_socket_t *owner, uint32_t reason,
                            int system_error, interop_linux_socket_work_t *work)
{
    int fd;
    interop_peer_event_t ignored;

    if (owner->closed)
        return;
    owner->cookie_current = false;
    if (owner->terminal_reason == InteropLinuxSocketTerminalNone)
        owner->terminal_reason = reason;
    if (owner->terminal_errno == 0 && system_error != 0)
        owner->terminal_errno = system_error;

    fd = owner->fd;
    owner->fd = -1;
    if (fd >= 0 && close(fd) != 0) {
        int close_error = errno;
        owner->close_errno = close_error;
        if (owner->terminal_errno == 0)
            owner->terminal_errno = close_error;
    }
    owner->closed = true;
    owner->state = InteropLinuxSocketTransportClosed;
    if (!owner->transport_notified && owner->peer != NULL &&
        (work == NULL || work->internal_steps < InteropLinuxSocketMaxInternalStepsPerDrive)) {
        if (work != NULL)
            ++work->internal_steps;
        clear_event(&ignored);
        if (interop_peer_transport_lost(owner->peer, owner->token, &ignored) == InteropPeerSuccess)
            owner->transport_notified = true;
        else {
            owner->peer_notify_failed = true;
            if (work != NULL)
                work->status = InteropLinuxSocketWorkPeerFailure;
        }
    }
}

/** @brief Fail closed after an unexpected peer ABI invariant. */
static void close_peer_failure(interop_linux_socket_t *owner,
                               interop_linux_socket_work_t *work)
{
    close_transport(owner, InteropLinuxSocketTerminalPeerFailure, 0, work);
}

/** @brief Record phase and first finite drain deadline from a peer result. */
static bool observe_phase(interop_linux_socket_t *owner, uint32_t phase,
                          uint64_t now_ms, const interop_peer_event_t *event)
{
    if (phase != InteropPeerAwaitHello &&
        phase != InteropPeerAwaitMapping &&
        phase != InteropPeerAwaitHelloAckSent &&
        phase != InteropPeerInitializingMapping &&
        phase != InteropPeerAwaitHelloSent &&
        phase != InteropPeerAwaitHelloAck &&
        phase != InteropPeerReady &&
        phase != InteropPeerDraining)
        return false;

    if (event != NULL && event->kind == InteropPeerEventDraining &&
        event->phase == InteropPeerDraining)
        save_terminal_event(owner, event);

    if (phase == InteropPeerDraining) {
        note_draining(owner, now_ms);
    } else if (phase == InteropPeerAwaitMapping ||
               phase == InteropPeerInitializingMapping) {
        owner->state = InteropLinuxSocketMappingWait;
    } else {
        owner->state = InteropLinuxSocketActive;
    }
    return true;
}

/** @brief Initialize a complete successful work record, including padding. */
static void init_work(interop_linux_socket_work_t *work,
                      const interop_linux_socket_t *owner)
{
    memset(work, 0, sizeof(*work));
    work->state = owner->state;
    work->terminal_reason = owner->terminal_reason;
    work->system_errno = owner->terminal_errno;
    if (owner->closed)
        copy_event(&work->diagnostic_event, &owner->terminal_event);
}

/** @brief Validate output storage and overlap with owner state. */
static bool valid_work_output(const interop_linux_socket_t *owner,
                              interop_linux_socket_work_t *work)
{
    return range_valid(work, sizeof(*work), _Alignof(interop_linux_socket_work_t)) &&
           output_disjoint_from_owner(owner, work, sizeof(*work));
}

/** @brief Invalidate any outstanding readiness snapshot. */
static void invalidate_cookie(interop_linux_socket_t *owner)
{
    owner->cookie_current = false;
}

/** @brief Close for an abort flag and save its peer event diagnostic. */
static void apply_abort_event(interop_linux_socket_t *owner,
                              const interop_peer_event_t *event,
                              interop_linux_socket_work_t *work)
{
    save_terminal_event(owner, event);
    close_transport(owner, InteropLinuxSocketTerminalPeerAbort, 0, work);
    if (work != NULL)
        work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                                   InteropLinuxSocketWorkTransportClosed;
}

/** @brief Commit a peer event to the work output and owner phase. */
static bool accept_peer_event(interop_linux_socket_t *owner,
                              const interop_peer_event_t *event,
                              interop_linux_socket_work_t *work,
                              uint64_t now_ms)
{
    bool repeated_draining = owner->state == InteropLinuxSocketDraining &&
                             event->kind == InteropPeerEventDraining &&
                             event->phase == InteropPeerDraining;

    if (!observe_phase(owner, event->phase, now_ms, event))
        return false;
    if (event->kind != InteropPeerEventNone && !repeated_draining)
        copy_event(&work->event, event);
    if (event->kind == InteropPeerEventValidateMapping)
        owner->mapping_callback_pending = true;
    else if (event->phase != InteropPeerAwaitMapping &&
             event->phase != InteropPeerInitializingMapping)
        owner->mapping_callback_pending = false;
    if ((event->flags & InteropPeerAbortTransport) != 0)
        apply_abort_event(owner, event, work);
    return true;
}

/** @brief Charge a bounded internal peer/helper operation. */
static bool take_internal_step(interop_linux_socket_work_t *work)
{
    if (work->internal_steps >= InteropLinuxSocketMaxInternalStepsPerDrive - 1u)
        return false;
    ++work->internal_steps;
    return true;
}

/** @brief Query the RX pending length using one accounted peer call. */
static int pending_length(interop_linux_socket_t *owner,
                          interop_linux_socket_work_t *work, size_t *length)
{
    if (!take_internal_step(work))
        return InternalWorkExhausted;
    if (interop_peer_pending_bytes(owner->peer, owner->token, length) != InteropPeerSuccess)
        return InteropLinuxSocketPeerFailure;
    return InteropLinuxSocketSuccess;
}

/** @brief Feed carry and optionally dispatch one complete peer frame. */
static int service_carry(interop_linux_socket_t *owner,
                         interop_linux_socket_work_t *work,
                         uint8_t *frame_out, uint64_t now_ms,
                         bool *frame_consumed)
{
    size_t available;
    size_t consumed = 0;
    size_t pending = 0;
    interop_peer_event_t event;
    int result;

    /* All call sites require Active, since peer feed is invalid in MappingWait
     * and Draining. */
    *frame_consumed = false;
    if (owner->rx_length != 0) {
        if (!take_internal_step(work))
            return InternalWorkExhausted;

        available = owner->rx_length;
        clear_event(&event);
        result = interop_peer_feed(owner->peer, owner->token,
                                   owner->rx_carry + owner->rx_start, available,
                                   &consumed, &event);
        if (result != InteropPeerSuccess)
            return InteropLinuxSocketPeerFailure;
        if (consumed > available)
            return InteropLinuxSocketPeerFailure;
        owner->rx_start += consumed;
        owner->rx_length -= consumed;
        if (consumed != 0)
            work->status = InteropLinuxSocketWorkProgress;
        if (owner->rx_length == 0)
            owner->rx_start = 0;

        if (event.kind != InteropPeerEventNone) {
            if (!accept_peer_event(owner, &event, work, now_ms))
                return InteropLinuxSocketPeerFailure;
            *frame_consumed = true;
            return InteropLinuxSocketSuccess;
        }
    }

    result = pending_length(owner, work, &pending);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (pending == 0)
        return InteropLinuxSocketSuccess;
    if (!take_internal_step(work))
        return InternalWorkExhausted;
    clear_event(&event);
    result = interop_peer_next_event(owner->peer, owner->token, now_ms,
                                     frame_out, InteropLinuxSocketFrameCapacity,
                                     &event);
    if (result != InteropPeerSuccess)
        return InteropLinuxSocketPeerFailure;
    if (!accept_peer_event(owner, &event, work, now_ms))
        return InteropLinuxSocketPeerFailure;
    if (!owner->closed)
        work->status = InteropLinuxSocketWorkProgress;
    *frame_consumed = true;
    return InteropLinuxSocketSuccess;
}

/** @brief Handle one fatal poll indicator before RX dispatch. */
static int handle_fatal_revents(interop_linux_socket_t *owner,
                                uint32_t revents,
                                interop_linux_socket_work_t *work)
{
    if ((revents & (uint32_t)POLLNVAL) != 0) {
        close_transport(owner, InteropLinuxSocketTerminalPollInvalid, EBADF, work);
        work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                                   InteropLinuxSocketWorkTransportClosed;
        return 1;
    }
    if (owner->poll_error_pending || (revents & (uint32_t)POLLERR) != 0) {
        int socket_error = 0;
        socklen_t error_length = (socklen_t)sizeof(socket_error);
        if (getsockopt(owner->fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &error_length) == 0) {
            owner->poll_error_pending = false;
            if (socket_error != 0) {
                close_transport(owner, InteropLinuxSocketTerminalSocketError,
                                socket_error, work);
                work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                                           InteropLinuxSocketWorkTransportClosed;
                return 1;
            }
        } else if (errno == EINTR) {
            owner->poll_error_pending = true;
            return 2; /* unresolved fatal evidence: defer all data dispatch */
        } else {
            int saved_error = errno;
            close_transport(owner, InteropLinuxSocketTerminalSocketError,
                            saved_error, work);
            work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                                       InteropLinuxSocketWorkTransportClosed;
            return 1;
        }
    }
    return 0;
}

/** @brief Compact live RX bytes to the beginning of the fixed carry. */
static void compact_rx(interop_linux_socket_t *owner)
{
    if (owner->rx_start != 0 && owner->rx_length != 0)
        memmove(owner->rx_carry, owner->rx_carry + owner->rx_start,
                owner->rx_length);
    owner->rx_start = 0;
}

/** @copydoc interop_linux_socket_create */
int interop_linux_socket_create(int connected_fd,
                                const interop_linux_socket_config_t *config,
                                interop_linux_socket_t **out_owner)
{
    interop_linux_socket_t *owner;
    struct sockaddr_storage peer_address;
    socklen_t peer_address_length = (socklen_t)sizeof(peer_address);
    int socket_type = 0;
    socklen_t socket_type_length = (socklen_t)sizeof(socket_type);
    int status_flags;
    int descriptor_flags;
    int peer_result;
    size_t version_length;

    if (config == NULL || out_owner == NULL || connected_fd < 0 ||
        !range_valid(config, sizeof(*config), _Alignof(interop_linux_socket_config_t)) ||
        !range_valid(out_owner, sizeof(*out_owner), _Alignof(interop_linux_socket_t *)) ||
        ranges_overlap(config, sizeof(*config), out_owner, sizeof(*out_owner)) ||
        *out_owner != NULL ||
        config->reserved_zero != 0 || config->drain_timeout_ms == 0 ||
        config->drain_timeout_ms > InteropLinuxSocketMaxDrainTimeoutMs)
        return InteropLinuxSocketInvalidArgument;

    version_length = config->peer_config.agent_version_bytes;
    if (config->peer_config.agent_version != NULL &&
        (!range_valid(config->peer_config.agent_version, version_length, 1) ||
         ranges_overlap(config->peer_config.agent_version, version_length,
                        out_owner, sizeof(*out_owner))))
        return InteropLinuxSocketInvalidArgument;

    if (getsockopt(connected_fd, SOL_SOCKET, SO_TYPE,
                   &socket_type, &socket_type_length) != 0 ||
        socket_type != SOCK_STREAM ||
        getpeername(connected_fd, (struct sockaddr *)(void *)&peer_address,
                    &peer_address_length) != 0)
        return InteropLinuxSocketInvalidArgument;
    status_flags = fcntl(connected_fd, F_GETFL);
    if (status_flags < 0 || (status_flags & O_NONBLOCK) == 0)
        return InteropLinuxSocketInvalidArgument;
    descriptor_flags = fcntl(connected_fd, F_GETFD);
    if (descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0)
        return InteropLinuxSocketInvalidArgument;

    owner = (interop_linux_socket_t *)calloc(1, sizeof(*owner));
    if (owner == NULL)
        return InteropLinuxSocketOutOfMemory;
    owner->fd = -1;
    owner->state = config->peer_config.role == InteropPeerGuest ?
                   InteropLinuxSocketMappingWait : InteropLinuxSocketActive;
    owner->owner_thread = pthread_self();
    owner->token = config->peer_config.connection_token;
    owner->last_ms = config->peer_config.accepted_at_ms;
    owner->drain_timeout_ms = config->drain_timeout_ms;
    owner->role = config->peer_config.role;
    owner->next_poll_cookie = 1;
    owner->mapping_callback_pending = config->peer_config.role == InteropPeerGuest;

    peer_result = interop_peer_create(&config->peer_config, &owner->peer);
    if (peer_result != InteropPeerSuccess) {
        free(owner);
        return map_peer_create_result(peer_result);
    }
    owner->fd = connected_fd;
    *out_owner = owner;
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_poll_events */
int interop_linux_socket_poll_events(interop_linux_socket_t *owner,
                                    uint64_t token, uint64_t now_ms,
                                    interop_linux_socket_poll_t *out_poll)
{
    interop_linux_socket_poll_t poll_result;
    interop_peer_event_t state_event;
    size_t tx_bytes = 0;
    size_t pending_bytes = 0;
    uint64_t cookie;
    int result;

    if (owner == NULL || out_poll == NULL ||
        !range_valid(out_poll, sizeof(*out_poll), _Alignof(interop_linux_socket_poll_t)) ||
        !output_disjoint_from_owner(owner, out_poll, sizeof(*out_poll)))
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    result = check_time(owner, now_ms);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (owner->cookies_exhausted)
        return InteropLinuxSocketPollCookieExhausted;

    memset(&poll_result, 0, sizeof(poll_result));
    if (owner->closed) {
        poll_result.fd = -1;
        poll_result.timeout_ms = 0;
    } else {
        clear_event(&state_event);
        if (interop_peer_get_state(owner->peer, owner->token, &state_event) != InteropPeerSuccess) {
            close_peer_failure(owner, NULL);
            return InteropLinuxSocketPeerFailure;
        }
        if (!observe_phase(owner, state_event.phase, now_ms, NULL)) {
            close_peer_failure(owner, NULL);
            return InteropLinuxSocketPeerFailure;
        }
        if (interop_peer_tx_bytes(owner->peer, owner->token, &tx_bytes) != InteropPeerSuccess ||
            interop_peer_pending_bytes(owner->peer, owner->token, &pending_bytes) != InteropPeerSuccess) {
            close_peer_failure(owner, NULL);
            return InteropLinuxSocketPeerFailure;
        }
        poll_result.fd = owner->fd;
        poll_result.events = (uint32_t)(POLLERR | POLLHUP | POLLNVAL);
        if ((owner->state == InteropLinuxSocketActive ||
             owner->state == InteropLinuxSocketMappingWait) &&
            owner->rx_length < sizeof(owner->rx_carry))
            poll_result.events |= (uint32_t)POLLIN;
        if (tx_bytes != 0 && owner->state != InteropLinuxSocketMappingWait)
            poll_result.events |= (uint32_t)POLLOUT;

        if ((owner->state == InteropLinuxSocketActive && owner->rx_length != 0) ||
            (owner->state == InteropLinuxSocketActive && pending_bytes != 0) ||
            (owner->state == InteropLinuxSocketDraining && tx_bytes == 0))
            poll_result.immediate_work = 1;
        poll_result.timeout_ms = InteropLinuxSocketMaxDriveIntervalMs;
        if (owner->state == InteropLinuxSocketDraining && owner->drain_deadline_set) {
            if (now_ms >= owner->drain_deadline_ms) {
                poll_result.immediate_work = 1;
                poll_result.timeout_ms = 0;
            } else {
                uint64_t remaining = owner->drain_deadline_ms - now_ms;
                if (remaining < poll_result.timeout_ms)
                    poll_result.timeout_ms = (uint32_t)remaining;
            }
        }
        if (poll_result.immediate_work != 0)
            poll_result.timeout_ms = 0;
    }

    cookie = owner->next_poll_cookie;
    if (cookie == 0) {
        owner->cookies_exhausted = true;
        return InteropLinuxSocketPollCookieExhausted;
    }
    poll_result.poll_cookie = cookie;
    if (cookie == UINT64_MAX) {
        owner->cookies_exhausted = true;
        owner->next_poll_cookie = 0;
    } else {
        owner->next_poll_cookie = cookie + 1;
    }
    owner->current_poll_cookie = cookie;
    owner->cookie_current = true;
    commit_time(owner, now_ms);
    memcpy(out_poll, &poll_result, sizeof(poll_result));
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_get_closed_snapshot */
int interop_linux_socket_get_closed_snapshot(interop_linux_socket_t *owner,
                                             uint64_t token,
                                             interop_linux_socket_work_t *out_work)
{
    interop_linux_socket_work_t work;
    int result;

    if (owner == NULL || out_work == NULL ||
        !valid_work_output(owner, out_work))
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (!owner->closed)
        return InteropLinuxSocketWrongPhase;
    init_work(&work, owner);
    work.status = InteropLinuxSocketWorkTransportClosed;
    memcpy(out_work, &work, sizeof(work));
    return InteropLinuxSocketSuccess;
}

/** @brief Terminalize an owner after an OS error while preparing drive output. */
static void terminalize_drive(interop_linux_socket_t *owner,
                              interop_linux_socket_work_t *work,
                              uint32_t reason, int saved_errno)
{
    close_transport(owner, reason, saved_errno, work);
    work->state = owner->state;
    work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                               InteropLinuxSocketWorkTransportClosed;
    work->terminal_reason = owner->terminal_reason;
    work->system_errno = owner->terminal_errno;
    copy_event(&work->diagnostic_event, &owner->terminal_event);
}

/** @brief Process an event and update terminal/phase fields in work. */
static bool accept_event_to_work(interop_linux_socket_t *owner,
                                 const interop_peer_event_t *event,
                                 interop_linux_socket_work_t *work,
                                 uint64_t now_ms)
{
    if (!accept_peer_event(owner, event, work, now_ms))
        return false;
    work->state = owner->state;
    work->terminal_reason = owner->terminal_reason;
    work->system_errno = owner->terminal_errno;
    if (owner->closed) {
        work->status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                                   InteropLinuxSocketWorkTransportClosed;
        copy_event(&work->diagnostic_event, &owner->terminal_event);
    } else if (event->kind != InteropPeerEventNone) {
        work->status = InteropLinuxSocketWorkProgress;
    }
    return true;
}

/** @brief Run the bounded drive loop for one current poll snapshot. */
/** @copydoc interop_linux_socket_drive */
int interop_linux_socket_drive(interop_linux_socket_t *owner, uint64_t token,
                               uint64_t poll_cookie, uint64_t now_ms,
                               uint32_t revents, size_t rx_byte_budget,
                               size_t tx_byte_budget, uint32_t syscall_budget,
                               uint8_t *frame_out, size_t frame_capacity,
                               interop_linux_socket_work_t *out_work)
{
    const uint32_t AllowedRevents = (uint32_t)(POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
    interop_linux_socket_work_t work;
    interop_peer_event_t event;
    uint32_t phase = 0;
    bool frame_consumed = false;
    size_t tx_budget_used = 0;
    size_t rx_budget_used = 0;
    int result;

    if (owner == NULL || out_work == NULL || frame_out == NULL ||
        frame_capacity < InteropLinuxSocketFrameCapacity ||
        !range_valid(out_work, sizeof(*out_work), _Alignof(interop_linux_socket_work_t)) ||
        !range_valid(frame_out, frame_capacity, 1) ||
        !output_disjoint_from_owner(owner, out_work, sizeof(*out_work)) ||
        !output_disjoint_from_owner(owner, frame_out, frame_capacity) ||
        ranges_overlap(out_work, sizeof(*out_work), frame_out, frame_capacity) ||
        rx_byte_budget > InteropLinuxSocketMaxRxBudgetBytes ||
        tx_byte_budget > InteropLinuxSocketMaxTxBudgetBytes ||
        syscall_budget > InteropLinuxSocketMaxSyscallBudget ||
        (revents & ~AllowedRevents) != 0)
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    result = check_time(owner, now_ms);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (!owner->cookie_current || poll_cookie == 0 || poll_cookie != owner->current_poll_cookie)
        return InteropLinuxSocketStalePoll;

    init_work(&work, owner);
    owner->cookie_current = false;
    commit_time(owner, now_ms);
    if (owner->closed) {
        work.status = InteropLinuxSocketWorkTransportClosed;
        work.event.kind = InteropPeerEventNone;
        memcpy(out_work, &work, sizeof(work));
        return InteropLinuxSocketSuccess;
    }

    if (!take_internal_step(&work))
        goto drive_done;
    clear_event(&event);
    result = interop_peer_tick(owner->peer, owner->token, now_ms, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, &work);
        work.status = InteropLinuxSocketWorkPeerFailure;
        work.state = owner->state;
        work.terminal_reason = owner->terminal_reason;
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        goto drive_done;
    }
    if (!accept_event_to_work(owner, &event, &work, now_ms)) {
        close_peer_failure(owner, &work);
        work.status = InteropLinuxSocketWorkPeerFailure;
        goto drive_done;
    }
    if (!owner->closed)
        work.status = InteropLinuxSocketWorkProgress;
    if (owner->closed)
        goto drive_done;
    result = handle_fatal_revents(owner, revents, &work);
    if (result != 0) {
        work.state = owner->state;
        work.terminal_reason = owner->terminal_reason;
        work.system_errno = owner->terminal_errno;
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        goto drive_done;
    }
    /* A newly surfaced event is the entire caller-visible unit for this
     * drive. Repeated Draining ticks are suppressed by accept_peer_event. */
    if (work.event.kind != InteropPeerEventNone || owner->closed)
        goto drive_done;

    while (owner->state == InteropLinuxSocketActive && owner->rx_length != 0) {
        result = service_carry(owner, &work, frame_out, now_ms, &frame_consumed);
        if (result == InteropLinuxSocketPeerFailure) {
            close_peer_failure(owner, &work);
            work.status = InteropLinuxSocketWorkPeerFailure;
            work.state = owner->state;
            work.terminal_reason = owner->terminal_reason;
            copy_event(&work.diagnostic_event, &owner->terminal_event);
            goto drive_done;
        }
        if (result == InternalWorkExhausted || frame_consumed || owner->closed)
            goto drive_done;
        if (owner->rx_length == 0)
            break;
    }

    /* A previous bounded drive may have fed the last carry byte but run out
     * of helper steps before extracting the peer's now-complete frame. The
     * peer assembler can therefore have pending bytes even with empty carry. */
    if (owner->state == InteropLinuxSocketActive && owner->rx_length == 0) {
        result = service_carry(owner, &work, frame_out, now_ms, &frame_consumed);
        if (result == InteropLinuxSocketPeerFailure) {
            close_peer_failure(owner, &work);
            work.status = InteropLinuxSocketWorkPeerFailure;
            work.state = owner->state;
            work.terminal_reason = owner->terminal_reason;
            copy_event(&work.diagnostic_event, &owner->terminal_event);
            goto drive_done;
        }
        if (result == InternalWorkExhausted || frame_consumed || owner->closed)
            goto drive_done;
    }

    if (owner->state == InteropLinuxSocketActive ||
        owner->state == InteropLinuxSocketMappingWait) {
        uint32_t revent_read = (uint32_t)(POLLIN | POLLHUP);
        while ((revents & revent_read) != 0 &&
               rx_budget_used < rx_byte_budget &&
               work.syscalls < syscall_budget &&
               owner->rx_length < sizeof(owner->rx_carry)) {
            size_t room;
            size_t amount;
            ssize_t received;

            compact_rx(owner);
            room = sizeof(owner->rx_carry) - owner->rx_length;
            amount = rx_byte_budget - rx_budget_used;
            if (amount > room)
                amount = room;
            if (amount == 0)
                break;
            ++work.syscalls;
            received = recv(owner->fd, owner->rx_carry + owner->rx_length,
                            amount, MSG_DONTWAIT);
            if (received > 0) {
                size_t received_size = (size_t)received;
                owner->rx_length += received_size;
                rx_budget_used += received_size;
                work.rx_bytes += received_size;
                work.status = InteropLinuxSocketWorkProgress;
                if (owner->state == InteropLinuxSocketActive) {
                    result = service_carry(owner, &work, frame_out, now_ms,
                                           &frame_consumed);
                    if (result == InteropLinuxSocketPeerFailure) {
                        close_peer_failure(owner, &work);
                        work.status = InteropLinuxSocketWorkPeerFailure;
                        work.state = owner->state;
                        work.terminal_reason = owner->terminal_reason;
                        copy_event(&work.diagnostic_event, &owner->terminal_event);
                        goto drive_done;
                    }
                    if (result == InternalWorkExhausted || frame_consumed ||
                        owner->closed)
                        goto drive_done;
                }
                continue;
            }
            if (received == 0) {
                terminalize_drive(owner, &work, InteropLinuxSocketTerminalEof, 0);
                goto drive_done;
            }
            if (errno == EINTR)
                continue;
            if (would_block(errno))
                break;
            terminalize_drive(owner, &work, InteropLinuxSocketTerminalSocketError, errno);
            goto drive_done;
        }
    }

    if (owner->closed)
        goto drive_done;
    if (owner->state == InteropLinuxSocketDraining) {
        size_t queued = 0;
        if (owner->drain_deadline_set && now_ms >= owner->drain_deadline_ms) {
            terminalize_drive(owner, &work, InteropLinuxSocketTerminalDrainDeadline, 0);
            goto drive_done;
        }
        if (!take_internal_step(&work))
            goto drive_done;
        if (interop_peer_tx_bytes(owner->peer, owner->token, &queued) != InteropPeerSuccess) {
            close_peer_failure(owner, &work);
            work.status = InteropLinuxSocketWorkPeerFailure;
            goto drive_done;
        }
        if (queued == 0) {
            terminalize_drive(owner, &work, InteropLinuxSocketTerminalDrainComplete, 0);
            goto drive_done;
        }
    }

    if (owner->state != InteropLinuxSocketMappingWait &&
        owner->state != InteropLinuxSocketTransportClosed &&
        tx_budget_used < tx_byte_budget && work.syscalls < syscall_budget &&
        (revents & (uint32_t)POLLOUT) != 0) {
        while (tx_budget_used < tx_byte_budget && work.syscalls < syscall_budget &&
               owner->state != InteropLinuxSocketTransportClosed) {
            size_t queued = 0;
            size_t remaining;
            size_t amount;
            ssize_t sent;
            interop_peer_event_t ack_event;

            if (owner->tx_offset >= owner->tx_length) {
                owner->tx_length = 0;
                owner->tx_offset = 0;
                if (!take_internal_step(&work))
                    break;
                if (interop_peer_tx_bytes(owner->peer, owner->token, &queued) != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropLinuxSocketWorkPeerFailure;
                    goto drive_done;
                }
                if (queued == 0)
                    break;
                if (!take_internal_step(&work))
                    break;
                if (interop_peer_copy_tx(owner->peer, owner->token,
                                         owner->tx_copy, sizeof(owner->tx_copy),
                                         &owner->tx_length) != InteropPeerSuccess ||
                    owner->tx_length == 0 || owner->tx_length != queued) {
                    close_peer_failure(owner, &work);
                    work.status = InteropLinuxSocketWorkPeerFailure;
                    goto drive_done;
                }
                owner->tx_offset = 0;
            }
            if ((InteropLinuxSocketMaxInternalStepsPerDrive - 1u) - work.internal_steps < 2u)
                break; /* reserve ACK and phase query before any positive send */
            remaining = owner->tx_length - owner->tx_offset;
            amount = tx_byte_budget - tx_budget_used;
            if (amount > remaining)
                amount = remaining;
            if (amount == 0)
                break;
            ++work.syscalls;
            sent = send(owner->fd, owner->tx_copy + owner->tx_offset,
                        amount, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) {
                size_t sent_size = (size_t)sent;
                work.tx_bytes += sent_size; /* accepted even if ACK then fails */
                tx_budget_used += sent_size;
                owner->tx_offset += sent_size;
                work.status = InteropLinuxSocketWorkProgress;
                if (!take_internal_step(&work))
                    break; /* impossible: two steps were reserved */
                clear_event(&ack_event);
                result = interop_peer_ack_tx(owner->peer, owner->token,
                                             now_ms, sent_size, &ack_event);
                if (result != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropLinuxSocketWorkPeerFailure;
                    work.state = owner->state;
                    work.terminal_reason = owner->terminal_reason;
                    work.system_errno = owner->terminal_errno;
                    copy_event(&work.diagnostic_event, &owner->terminal_event);
                    goto drive_done;
                }
                if (!take_internal_step(&work))
                    break; /* reserved phase step */
                clear_event(&event);
                result = interop_peer_get_state(owner->peer, owner->token, &event);
                if (result != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropLinuxSocketWorkPeerFailure;
                    goto drive_done;
                }
                phase = event.phase;
                if (!observe_phase(owner, phase, now_ms, &ack_event)) {
                    close_peer_failure(owner, &work);
                    work.status = InteropLinuxSocketWorkPeerFailure;
                    goto drive_done;
                }
                if (owner->tx_offset == owner->tx_length) {
                    owner->tx_offset = 0;
                    owner->tx_length = 0;
                }
                if (ack_event.kind != InteropPeerEventNone) {
                    if (!accept_event_to_work(owner, &ack_event, &work, now_ms)) {
                        close_peer_failure(owner, &work);
                        work.status = InteropLinuxSocketWorkPeerFailure;
                    }
                    goto drive_done;
                }
                if (owner->state == InteropLinuxSocketDraining &&
                    owner->drain_deadline_set && now_ms >= owner->drain_deadline_ms) {
                    terminalize_drive(owner, &work, InteropLinuxSocketTerminalDrainDeadline, 0);
                    goto drive_done;
                }
                continue;
            }
            if (sent == 0) {
                terminalize_drive(owner, &work, InteropLinuxSocketTerminalSocketError, EPIPE);
                goto drive_done;
            }
            if (errno == EINTR)
                continue;
            if (would_block(errno))
                break;
            terminalize_drive(owner, &work, InteropLinuxSocketTerminalSocketError, errno);
            goto drive_done;
        }
    }

drive_done:
    if (work.status == InteropLinuxSocketWorkIdle &&
        (work.rx_bytes != 0 || work.tx_bytes != 0 || work.event.kind != InteropPeerEventNone))
        work.status = InteropLinuxSocketWorkProgress;
    work.state = owner->state;
    work.terminal_reason = owner->terminal_reason;
    work.system_errno = owner->terminal_errno;
    if (owner->closed) {
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        if (owner->peer_notify_failed)
            work.status = InteropLinuxSocketWorkPeerFailure;
        else if (work.status == InteropLinuxSocketWorkIdle)
            work.status = InteropLinuxSocketWorkTransportClosed;
    }
    memcpy(out_work, &work, sizeof(work));
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_mapping_result */
int interop_linux_socket_mapping_result(interop_linux_socket_t *owner,
                                       uint64_t token, uint64_t now_ms,
                                       uint64_t epoch, uint32_t layout_version,
                                       uint32_t succeeded,
                                       interop_peer_event_t *out_event)
{
    interop_peer_event_t event;
    int result;

    if (owner == NULL || out_event == NULL ||
        !range_valid(out_event, sizeof(*out_event), _Alignof(interop_peer_event_t)) ||
        !output_disjoint_from_owner(owner, out_event, sizeof(*out_event)) ||
        succeeded > 1)
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (owner->closed)
        return InteropLinuxSocketClosed;
    result = check_time(owner, now_ms);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (owner->state != InteropLinuxSocketMappingWait || !owner->mapping_callback_pending)
        return InteropLinuxSocketWrongPhase;

    /* Mirror only the Guest callback's documented caller-input failures.
     * Host validation inputs, including epoch/layout mismatch, are evidence
     * for the peer and must be forwarded so it can enter Draining. */
    if (owner->role == InteropPeerGuest) {
        if (layout_version != InteropLayoutVersion ||
            (succeeded == 0) || (epoch == 0))
            return succeeded == 0 && layout_version == InteropLayoutVersion ?
                   InteropLinuxSocketMappingFailed : InteropLinuxSocketInvalidArgument;
    }

    clear_event(&event);
    result = interop_peer_mapping_result(owner->peer, owner->token, now_ms,
                                         epoch, layout_version, succeeded, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, NULL);
        return InteropLinuxSocketPeerFailure;
    }

    commit_time(owner, now_ms);
    owner->mapping_callback_pending = false;
    invalidate_cookie(owner); /* every successful non-drive mapping result */
    if (!observe_phase(owner, event.phase, now_ms, &event)) {
        close_peer_failure(owner, NULL);
        return InteropLinuxSocketPeerFailure;
    }
    if ((event.flags & InteropPeerAbortTransport) != 0)
        apply_abort_event(owner, &event, NULL);
    copy_event(out_event, &event);
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_begin_drain */
int interop_linux_socket_begin_drain(interop_linux_socket_t *owner,
                                     uint64_t token, uint64_t now_ms,
                                     interop_peer_event_t *out_event)
{
    interop_peer_event_t event;
    int result;

    if (owner == NULL || out_event == NULL ||
        !range_valid(out_event, sizeof(*out_event), _Alignof(interop_peer_event_t)) ||
        !output_disjoint_from_owner(owner, out_event, sizeof(*out_event)))
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    if (owner->closed)
        return InteropLinuxSocketClosed;
    result = check_time(owner, now_ms);
    if (result != InteropLinuxSocketSuccess)
        return result;

    clear_event(&event);
    result = interop_peer_begin_drain(owner->peer, owner->token, now_ms, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, NULL);
        return InteropLinuxSocketPeerFailure;
    }

    commit_time(owner, now_ms);
    invalidate_cookie(owner); /* all successful drain operations invalidate */
    if (!observe_phase(owner, event.phase, now_ms, &event)) {
        close_peer_failure(owner, NULL);
        return InteropLinuxSocketPeerFailure;
    }
    if ((event.flags & InteropPeerAbortTransport) != 0)
        apply_abort_event(owner, &event, NULL);
    copy_event(out_event, &event);
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_abort */
int interop_linux_socket_abort(interop_linux_socket_t *owner, uint64_t token,
                               interop_linux_socket_work_t *out_work)
{
    interop_linux_socket_work_t work;
    int result;

    if (owner == NULL || out_work == NULL || !valid_work_output(owner, out_work))
        return InteropLinuxSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropLinuxSocketSuccess)
        return result;
    init_work(&work, owner);
    invalidate_cookie(owner); /* every successful abort invalidates */
    if (owner->closed) {
        work.status = InteropLinuxSocketWorkTransportClosed;
        memcpy(out_work, &work, sizeof(work));
        return InteropLinuxSocketSuccess;
    }
    close_transport(owner, InteropLinuxSocketTerminalLocalAbort, 0, &work);
    work.state = owner->state;
    work.status = owner->peer_notify_failed ? InteropLinuxSocketWorkPeerFailure :
                                               InteropLinuxSocketWorkTransportClosed;
    work.terminal_reason = owner->terminal_reason;
    work.system_errno = owner->terminal_errno;
    work.event.kind = InteropPeerEventNone;
    copy_event(&work.diagnostic_event, &owner->terminal_event);
    memcpy(out_work, &work, sizeof(work));
    return InteropLinuxSocketSuccess;
}

/** @copydoc interop_linux_socket_destroy */
int interop_linux_socket_destroy(interop_linux_socket_t **owner_slot)
{
    interop_linux_socket_t *owner;
    int close_error;
    int peer_result;

    if (owner_slot == NULL ||
        !range_valid(owner_slot, sizeof(*owner_slot), _Alignof(interop_linux_socket_t *)))
        return InteropLinuxSocketInvalidArgument;
    owner = *owner_slot;
    if (owner == NULL)
        return InteropLinuxSocketSuccess;
    if (!on_owner_thread(owner))
        return InteropLinuxSocketWrongThread;
    if (ranges_overlap(owner_slot, sizeof(*owner_slot), owner, sizeof(*owner)))
        return InteropLinuxSocketInvalidArgument;

    if (!owner->closed)
        close_transport(owner, InteropLinuxSocketTerminalDestroyed, 0, NULL);
    close_error = owner->close_errno;
    if (owner->peer != NULL && !owner->transport_notified) {
        interop_peer_event_t ignored;
        clear_event(&ignored);
        peer_result = interop_peer_transport_lost(owner->peer, owner->token, &ignored);
        if (peer_result != InteropPeerSuccess)
            return InteropLinuxSocketPeerFailure;
        owner->transport_notified = true;
    }
    if (owner->peer != NULL) {
        peer_result = interop_peer_destroy(&owner->peer);
        if (peer_result != InteropPeerSuccess)
            return InteropLinuxSocketPeerFailure;
    }
    memset(owner, 0, sizeof(*owner));
    free(owner);
    *owner_slot = NULL;
    if (close_error != 0) {
        errno = close_error;
        return InteropLinuxSocketCloseDiagnostic;
    }
    return InteropLinuxSocketSuccess;
}
