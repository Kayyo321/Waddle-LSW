/**
 * @file windows_socket_owner.c
 * @brief Bounded pull-driven Windows stream transport owner for interop_peer_t.
 *
 * @ownership The owner exclusively owns its accepted SOCKET and peer after create
 * succeeds. RX/TX arrays are embedded. No caller pointer or callback is saved.
 * @thread_safety Every operation is serialized on the GetCurrentThreadId that created it.
 */
#if defined(InteropWindowsSocketTestApi)
#define InteropWindowsSocketOwnerImplementation 1
#endif
#include "windows_socket_owner.h"
#include "waddle/interop_protocol.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(InteropWindowsSocketTestApi)
#include "windows_socket_owner_api_double.h"
#else
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

enum { InternalWorkExhausted = -100 };

_Static_assert(sizeof(interop_windows_socket_config_t) == 56, "Windows socket config ABI");
_Static_assert(sizeof(interop_windows_socket_poll_t) == 32, "Windows socket poll ABI");
_Static_assert(sizeof(interop_windows_socket_work_t) == 160, "Windows socket work ABI");
_Static_assert(sizeof(interop_windows_socket_quarantine_t) == 32, "Windows socket quarantine ABI");

struct interop_windows_socket_t {
    SOCKET socket_value;
    DWORD owner_thread_id;
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
    uint32_t terminal_wsa_error;
    uint32_t close_wsa_error;
    bool cookie_current;
    bool cookies_exhausted;
    bool drain_deadline_set;
    bool mapping_callback_pending;
    bool transport_notified;
    bool poll_error_pending;
    bool peer_notify_failed;
    bool notify_deferred;
    bool closed;
    bool close_indeterminate;
    interop_peer_event_t terminal_event;
    uint8_t rx_carry[InteropWindowsSocketRxCarryBytes];
    size_t rx_start;
    size_t rx_length;
    uint8_t tx_copy[InteropWindowsSocketFrameCapacity];
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
static bool output_disjoint_from_owner(const interop_windows_socket_t *owner,
                                      const void *output, size_t output_length)
{
    return !ranges_overlap(owner, sizeof(*owner), output, output_length);
}

/** @brief Check owner-thread affinity without changing owner state. */
static bool on_owner_thread(const interop_windows_socket_t *owner)
{
    return owner->owner_thread_id == GetCurrentThreadId();
}

/** @brief Match nonblocking readiness on platforms with aliased WSAGetLastError() values. */
static bool would_block(int error_code)
{
    return error_code == WSAEWOULDBLOCK;
}

/** @brief Check token before reading protocol state or caller time. */
static int check_owner_token(interop_windows_socket_t *owner, uint64_t token)
{
    if (!on_owner_thread(owner))
        return InteropWindowsSocketWrongThread;
    if (token == 0 || token != owner->token)
        return InteropWindowsSocketWrongToken;
    return InteropWindowsSocketSuccess;
}

/** @brief Check monotonic time without consuming it. */
static int check_time(const interop_windows_socket_t *owner, uint64_t now_ms)
{
    if (now_ms < owner->last_ms)
        return InteropWindowsSocketTimeWentBackwards;
    return InteropWindowsSocketSuccess;
}

/** @brief Commit a caller timestamp after successful preflight/operation. */
static void commit_time(interop_windows_socket_t *owner, uint64_t now_ms)
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
        return InteropWindowsSocketInvalidArgument;
    case InteropPeerOutOfMemory:
        return InteropWindowsSocketOutOfMemory;
    default:
        return InteropWindowsSocketPeerFailure;
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
static void note_draining(interop_windows_socket_t *owner, uint64_t now_ms)
{
    owner->state = InteropWindowsSocketDraining;
    if (!owner->drain_deadline_set) {
        owner->drain_deadline_ms = saturating_deadline(now_ms, owner->drain_timeout_ms);
        owner->drain_deadline_set = true;
    }
}

/** @brief Save the first terminal peer event without replaying it later. */
static void save_terminal_event(interop_windows_socket_t *owner,
                                const interop_peer_event_t *event)
{
    if (owner->terminal_event.kind == InteropPeerEventNone &&
        event->kind == InteropPeerEventDraining)
        copy_event(&owner->terminal_event, event);
}

/** @brief Notify the peer once after native close has succeeded. */
static void notify_transport_lost(interop_windows_socket_t *owner,
                                  interop_windows_socket_work_t *work)
{
    interop_peer_event_t ignored;

    if (owner->transport_notified || owner->peer_notify_failed || owner->peer == NULL ||
        owner->state != InteropWindowsSocketTransportClosed)
        return;
    if (work != NULL && work->internal_steps >= InteropWindowsSocketMaxInternalStepsPerDrive) {
        owner->notify_deferred = true;
        return;
    }
    if (work != NULL)
        ++work->internal_steps;
    owner->notify_deferred = false;
    clear_event(&ignored);
    if (interop_peer_transport_lost(owner->peer, owner->token, &ignored) == InteropPeerSuccess)
        owner->transport_notified = true;
    else {
        owner->peer_notify_failed = true;
        if (work != NULL)
            work->status = InteropWindowsSocketWorkPeerFailure;
    }
}

/** @brief Close an owned socket once; defer peer notification if work is exhausted. */
static void close_transport(interop_windows_socket_t *owner, uint32_t reason,
                            uint32_t system_error, interop_windows_socket_work_t *work)
{
    int close_result;
    uint32_t close_error;

    if (owner->closed)
        return;
    owner->cookie_current = false;
    if (owner->terminal_reason == InteropWindowsSocketTerminalNone)
        owner->terminal_reason = reason;
    if (owner->terminal_wsa_error == 0 && system_error != 0)
        owner->terminal_wsa_error = system_error;

    close_result = closesocket(owner->socket_value);
    if (close_result == SOCKET_ERROR) {
        close_error = (uint32_t)WSAGetLastError();
        owner->close_wsa_error = close_error;
        if (owner->terminal_wsa_error == 0)
            owner->terminal_wsa_error = close_error;
        owner->closed = true;
        owner->state = close_error == WSAEWOULDBLOCK ?
                       InteropWindowsSocketClosePendingState :
                       InteropWindowsSocketCloseIndeterminateState;
        owner->close_indeterminate = close_error != WSAEWOULDBLOCK;
        if (work != NULL)
            work->close_wsa_error = close_error;
        return;
    }
    owner->socket_value = INVALID_SOCKET;
    owner->closed = true;
    owner->state = InteropWindowsSocketTransportClosed;
    if (work != NULL)
        work->close_wsa_error = 0;
    notify_transport_lost(owner, work);
}

/** @brief Fail closed after an unexpected peer ABI invariant. */
static void close_peer_failure(interop_windows_socket_t *owner,
                               interop_windows_socket_work_t *work)
{
    close_transport(owner, InteropWindowsSocketTerminalPeerFailure, 0, work);
}

/** @brief Record phase and first finite drain deadline from a peer result. */
static bool observe_phase(interop_windows_socket_t *owner, uint32_t phase,
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
        owner->state = InteropWindowsSocketMappingWait;
    } else {
        owner->state = InteropWindowsSocketActive;
    }
    return true;
}

/** @brief Initialize a complete successful work record, including padding. */
static void init_work(interop_windows_socket_work_t *work,
                      const interop_windows_socket_t *owner)
{
    memset(work, 0, sizeof(*work));
    work->state = owner->state;
    work->terminal_reason = owner->terminal_reason;
    work->wsa_error = owner->terminal_wsa_error;
    work->close_wsa_error = owner->close_wsa_error;
    if (owner->closed)
        copy_event(&work->diagnostic_event, &owner->terminal_event);
}

/** @brief Translate the terminal native-close state to a copied work status. */
static uint32_t close_work_status(const interop_windows_socket_t *owner)
{
    if (owner->peer_notify_failed)
        return InteropWindowsSocketWorkPeerFailure;
    if (owner->state == InteropWindowsSocketTransportClosed)
        return InteropWindowsSocketWorkTransportClosed;
    if (owner->state == InteropWindowsSocketClosePendingState)
        return InteropWindowsSocketWorkClosePending;
    if (owner->state == InteropWindowsSocketCloseIndeterminateState)
        return InteropWindowsSocketWorkCloseIndeterminate;
    return InteropWindowsSocketWorkIdle;
}

/** @brief Validate output storage and overlap with owner state. */
static bool valid_work_output(const interop_windows_socket_t *owner,
                              interop_windows_socket_work_t *work)
{
    return range_valid(work, sizeof(*work), _Alignof(interop_windows_socket_work_t)) &&
           output_disjoint_from_owner(owner, work, sizeof(*work));
}

/** @brief Invalidate any outstanding readiness snapshot. */
static void invalidate_cookie(interop_windows_socket_t *owner)
{
    owner->cookie_current = false;
}

/** @brief Close for an abort flag and save its peer event diagnostic. */
static void apply_abort_event(interop_windows_socket_t *owner,
                              const interop_peer_event_t *event,
                              interop_windows_socket_work_t *work)
{
    save_terminal_event(owner, event);
    close_transport(owner, InteropWindowsSocketTerminalPeerAbort, 0, work);
    if (work != NULL)
        work->status = close_work_status(owner);
}

/** @brief Commit a peer event to the work output and owner phase. */
static bool accept_peer_event(interop_windows_socket_t *owner,
                              const interop_peer_event_t *event,
                              interop_windows_socket_work_t *work,
                              uint64_t now_ms)
{
    bool repeated_draining = owner->state == InteropWindowsSocketDraining &&
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
static bool take_internal_step(interop_windows_socket_work_t *work)
{
    if (work->internal_steps >= InteropWindowsSocketMaxInternalStepsPerDrive - 1u)
        return false;
    ++work->internal_steps;
    return true;
}

/** @brief Query the RX pending length using one accounted peer call. */
static int pending_length(interop_windows_socket_t *owner,
                          interop_windows_socket_work_t *work, size_t *length)
{
    if (!take_internal_step(work))
        return InternalWorkExhausted;
    if (interop_peer_pending_bytes(owner->peer, owner->token, length) != InteropPeerSuccess)
        return InteropWindowsSocketPeerFailure;
    return InteropWindowsSocketSuccess;
}

/** @brief Feed carry and optionally dispatch one complete peer frame. */
static int service_carry(interop_windows_socket_t *owner,
                         interop_windows_socket_work_t *work,
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
            return InteropWindowsSocketPeerFailure;
        if (consumed > available)
            return InteropWindowsSocketPeerFailure;
        owner->rx_start += consumed;
        owner->rx_length -= consumed;
        if (consumed != 0)
            work->status = InteropWindowsSocketWorkProgress;
        if (owner->rx_length == 0)
            owner->rx_start = 0;

        if (event.kind != InteropPeerEventNone) {
            if (!accept_peer_event(owner, &event, work, now_ms))
                return InteropWindowsSocketPeerFailure;
            *frame_consumed = true;
            return InteropWindowsSocketSuccess;
        }
    }

    result = pending_length(owner, work, &pending);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (pending == 0)
        return InteropWindowsSocketSuccess;
    if (!take_internal_step(work))
        return InternalWorkExhausted;
    clear_event(&event);
    result = interop_peer_next_event(owner->peer, owner->token, now_ms,
                                     frame_out, InteropWindowsSocketFrameCapacity,
                                     &event);
    if (result != InteropPeerSuccess)
        return InteropWindowsSocketPeerFailure;
    if (!accept_peer_event(owner, &event, work, now_ms))
        return InteropWindowsSocketPeerFailure;
    if (!owner->closed)
        work->status = InteropWindowsSocketWorkProgress;
    *frame_consumed = true;
    return InteropWindowsSocketSuccess;
}

/** @brief Handle one fatal poll indicator before RX dispatch. */
static int handle_fatal_revents(interop_windows_socket_t *owner,
                                uint32_t revents,
                                interop_windows_socket_work_t *work)
{
    if ((revents & (uint32_t)POLLNVAL) != 0) {
        close_transport(owner, InteropWindowsSocketTerminalPollInvalid, WSAENOTSOCK, work);
        work->status = close_work_status(owner);
        return 1;
    }
    if (owner->poll_error_pending || (revents & (uint32_t)POLLERR) != 0) {
        int socket_error = 0;
        int error_length = (int)sizeof(socket_error);
        if (getsockopt(owner->socket_value, SOL_SOCKET, SO_ERROR,
                       (char *)&socket_error, &error_length) == 0) {
            owner->poll_error_pending = false;
            if (socket_error != 0) {
                close_transport(owner, InteropWindowsSocketTerminalSocketError,
                                socket_error, work);
                work->status = close_work_status(owner);
                return 1;
            }
        } else if (WSAGetLastError() == WSAEINTR) {
            owner->poll_error_pending = true;
            return 2; /* unresolved fatal evidence: defer all data dispatch */
        } else {
            int saved_error = WSAGetLastError();
            close_transport(owner, InteropWindowsSocketTerminalSocketError,
                            saved_error, work);
            work->status = close_work_status(owner);
            return 1;
        }
    }
    return 0;
}

/** @brief Compact live RX bytes to the beginning of the fixed carry. */
static void compact_rx(interop_windows_socket_t *owner)
{
    if (owner->rx_start != 0 && owner->rx_length != 0)
        memmove(owner->rx_carry, owner->rx_carry + owner->rx_start,
                owner->rx_length);
    owner->rx_start = 0;
}

/** @copydoc interop_windows_socket_create */
int interop_windows_socket_create(SOCKET connected_socket,
                                const interop_windows_socket_config_t *config,
                                interop_windows_socket_t **out_owner)
{
    interop_windows_socket_t *owner;
    struct sockaddr_storage peer_address;
    int peer_address_length = (int)sizeof(peer_address);
    int socket_type = 0;
    int socket_type_length = (int)sizeof(socket_type);
    DWORD handle_flags = 0;
    int option_length;
    struct linger linger_value;
    u_long nonblocking = 1;
    int peer_result;
    size_t version_length;

    if (config == NULL || out_owner == NULL || connected_socket == INVALID_SOCKET ||
        !range_valid(config, sizeof(*config), _Alignof(interop_windows_socket_config_t)) ||
        !range_valid(out_owner, sizeof(*out_owner), _Alignof(interop_windows_socket_t *)) ||
        ranges_overlap(config, sizeof(*config), out_owner, sizeof(*out_owner)) ||
        *out_owner != NULL ||
        config->reserved_zero != 0 || config->drain_timeout_ms == 0 ||
        config->drain_timeout_ms > InteropWindowsSocketMaxDrainTimeoutMs)
        return InteropWindowsSocketInvalidArgument;

    version_length = config->peer_config.agent_version_bytes;
    if (config->peer_config.agent_version != NULL &&
        (!range_valid(config->peer_config.agent_version, version_length, 1) ||
         ranges_overlap(config->peer_config.agent_version, version_length,
                        out_owner, sizeof(*out_owner))))
        return InteropWindowsSocketInvalidArgument;

    if (getsockopt(connected_socket, SOL_SOCKET, SO_TYPE,
                   (char *)&socket_type, &socket_type_length) != 0 ||
        socket_type_length != (int)sizeof(socket_type) || socket_type != SOCK_STREAM ||
        getpeername(connected_socket, (struct sockaddr *)(void *)&peer_address,
                    &peer_address_length) != 0 ||
        peer_address_length < (int)sizeof(((struct sockaddr *)0)->sa_family) ||
        peer_address_length > (int)sizeof(peer_address))
        return InteropWindowsSocketInvalidArgument;
    if (!GetHandleInformation((HANDLE)(uintptr_t)connected_socket, &handle_flags) ||
        (handle_flags & HANDLE_FLAG_INHERIT) != 0)
        return InteropWindowsSocketInvalidArgument;
    option_length = (int)sizeof(linger_value);
    if (getsockopt(connected_socket, SOL_SOCKET, SO_LINGER,
                   (char *)&linger_value, &option_length) != 0 ||
        option_length != (int)sizeof(linger_value) || linger_value.l_onoff != 0)
        return InteropWindowsSocketInvalidArgument;

    owner = (interop_windows_socket_t *)calloc(1, sizeof(*owner));
    if (owner == NULL)
        return InteropWindowsSocketOutOfMemory;
    owner->socket_value = INVALID_SOCKET;
    owner->state = config->peer_config.role == InteropPeerGuest ?
                   InteropWindowsSocketMappingWait : InteropWindowsSocketActive;
    owner->owner_thread_id = GetCurrentThreadId();
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
    if (ioctlsocket(connected_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        uint32_t saved_error = (uint32_t)WSAGetLastError();
        (void)interop_peer_destroy(&owner->peer);
        free(owner);
        return saved_error == WSAENOBUFS ? InteropWindowsSocketOutOfMemory :
                                           InteropWindowsSocketOsError;
    }
    owner->socket_value = connected_socket;
    *out_owner = owner;
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_poll_events */
int interop_windows_socket_poll_events(interop_windows_socket_t *owner,
                                    uint64_t token, uint64_t now_ms,
                                    interop_windows_socket_poll_t *out_poll)
{
    interop_windows_socket_poll_t poll_result;
    interop_peer_event_t state_event;
    size_t tx_bytes = 0;
    size_t pending_bytes = 0;
    uint64_t cookie;
    int result;

    if (owner == NULL || out_poll == NULL ||
        !range_valid(out_poll, sizeof(*out_poll), _Alignof(interop_windows_socket_poll_t)) ||
        !output_disjoint_from_owner(owner, out_poll, sizeof(*out_poll)))
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    result = check_time(owner, now_ms);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->cookies_exhausted)
        return InteropWindowsSocketPollCookieExhausted;

    memset(&poll_result, 0, sizeof(poll_result));
    if (owner->closed) {
        if (owner->notify_deferred)
            notify_transport_lost(owner, NULL);
        poll_result.socket_value = 0;
        poll_result.timeout_ms = 0;
    } else {
        clear_event(&state_event);
        if (interop_peer_get_state(owner->peer, owner->token, &state_event) != InteropPeerSuccess) {
            close_peer_failure(owner, NULL);
            return InteropWindowsSocketPeerFailure;
        }
        if (!observe_phase(owner, state_event.phase, now_ms, NULL)) {
            close_peer_failure(owner, NULL);
            return InteropWindowsSocketPeerFailure;
        }
        if (interop_peer_tx_bytes(owner->peer, owner->token, &tx_bytes) != InteropPeerSuccess ||
            interop_peer_pending_bytes(owner->peer, owner->token, &pending_bytes) != InteropPeerSuccess) {
            close_peer_failure(owner, NULL);
            return InteropWindowsSocketPeerFailure;
        }
        poll_result.socket_value = (uintptr_t)owner->socket_value;
        poll_result.events = (uint32_t)(POLLERR | POLLHUP | POLLNVAL);
        if ((owner->state == InteropWindowsSocketActive ||
             owner->state == InteropWindowsSocketMappingWait) &&
            owner->rx_length < sizeof(owner->rx_carry))
            poll_result.events |= (uint32_t)POLLRDNORM;
        if (tx_bytes != 0 && owner->state != InteropWindowsSocketMappingWait)
            poll_result.events |= (uint32_t)POLLWRNORM;

        if ((owner->state == InteropWindowsSocketActive && owner->rx_length != 0) ||
            (owner->state == InteropWindowsSocketActive && pending_bytes != 0) ||
            (owner->state == InteropWindowsSocketDraining && tx_bytes == 0))
            poll_result.immediate_work = 1;
        poll_result.timeout_ms = InteropWindowsSocketMaxDriveIntervalMs;
        if (owner->state == InteropWindowsSocketDraining && owner->drain_deadline_set) {
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
        return InteropWindowsSocketPollCookieExhausted;
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
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_get_closed_snapshot */
int interop_windows_socket_get_closed_snapshot(interop_windows_socket_t *owner,
                                             uint64_t token,
                                             interop_windows_socket_work_t *out_work)
{
    interop_windows_socket_work_t work;
    int result;

    if (owner == NULL || out_work == NULL ||
        !valid_work_output(owner, out_work))
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->state != InteropWindowsSocketTransportClosed)
        return InteropWindowsSocketWrongPhase;
    init_work(&work, owner);
    work.status = InteropWindowsSocketWorkTransportClosed;
    memcpy(out_work, &work, sizeof(work));
    return InteropWindowsSocketSuccess;
}

/** @brief Terminalize an owner after an OS error while preparing drive output. */
static void terminalize_drive(interop_windows_socket_t *owner,
                              interop_windows_socket_work_t *work,
                              uint32_t reason, int saved_error)
{
    close_transport(owner, reason, (uint32_t)saved_error, work);
    work->state = owner->state;
    work->status = close_work_status(owner);
    work->terminal_reason = owner->terminal_reason;
    work->wsa_error = owner->terminal_wsa_error;
    copy_event(&work->diagnostic_event, &owner->terminal_event);
}

/** @brief Process an event and update terminal/phase fields in work. */
static bool accept_event_to_work(interop_windows_socket_t *owner,
                                 const interop_peer_event_t *event,
                                 interop_windows_socket_work_t *work,
                                 uint64_t now_ms)
{
    if (!accept_peer_event(owner, event, work, now_ms))
        return false;
    work->state = owner->state;
    work->terminal_reason = owner->terminal_reason;
    work->wsa_error = owner->terminal_wsa_error;
    if (owner->closed) {
        work->status = close_work_status(owner);
        copy_event(&work->diagnostic_event, &owner->terminal_event);
    } else if (event->kind != InteropPeerEventNone) {
        work->status = InteropWindowsSocketWorkProgress;
    }
    return true;
}

/** @brief Run the bounded drive loop for one current poll snapshot. */
/** @copydoc interop_windows_socket_drive */
int interop_windows_socket_drive(interop_windows_socket_t *owner, uint64_t token,
                               uint64_t poll_cookie, uint64_t now_ms,
                               uint32_t revents, size_t rx_byte_budget,
                               size_t tx_byte_budget, uint32_t syscall_budget,
                               uint8_t *frame_out, size_t frame_capacity,
                               interop_windows_socket_work_t *out_work)
{
    const uint32_t AllowedRevents = (uint32_t)(POLLRDNORM | POLLWRNORM | POLLERR | POLLHUP | POLLNVAL);
    interop_windows_socket_work_t work;
    interop_peer_event_t event;
    uint32_t phase = 0;
    bool frame_consumed = false;
    size_t tx_budget_used = 0;
    size_t rx_budget_used = 0;
    int result;

    if (owner == NULL || out_work == NULL || frame_out == NULL ||
        frame_capacity < InteropWindowsSocketFrameCapacity ||
        !range_valid(out_work, sizeof(*out_work), _Alignof(interop_windows_socket_work_t)) ||
        !range_valid(frame_out, frame_capacity, 1) ||
        !output_disjoint_from_owner(owner, out_work, sizeof(*out_work)) ||
        !output_disjoint_from_owner(owner, frame_out, frame_capacity) ||
        ranges_overlap(out_work, sizeof(*out_work), frame_out, frame_capacity) ||
        rx_byte_budget > InteropWindowsSocketMaxRxBudgetBytes ||
        tx_byte_budget > InteropWindowsSocketMaxTxBudgetBytes ||
        syscall_budget > InteropWindowsSocketMaxSyscallBudget ||
        (revents & ~AllowedRevents) != 0)
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    result = check_time(owner, now_ms);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (!owner->cookie_current || poll_cookie == 0 || poll_cookie != owner->current_poll_cookie)
        return InteropWindowsSocketStalePoll;

    init_work(&work, owner);
    owner->cookie_current = false;
    commit_time(owner, now_ms);
    if (owner->closed) {
        if (owner->notify_deferred)
            notify_transport_lost(owner, &work);
        work.status = close_work_status(owner);
        work.event.kind = InteropPeerEventNone;
        if (owner->peer_notify_failed)
            work.status = InteropWindowsSocketWorkPeerFailure;
        memcpy(out_work, &work, sizeof(work));
        return InteropWindowsSocketSuccess;
    }

    if (!take_internal_step(&work))
        goto drive_done;
    clear_event(&event);
    result = interop_peer_tick(owner->peer, owner->token, now_ms, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, &work);
        work.status = InteropWindowsSocketWorkPeerFailure;
        work.state = owner->state;
        work.terminal_reason = owner->terminal_reason;
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        goto drive_done;
    }
    if (!accept_event_to_work(owner, &event, &work, now_ms)) {
        close_peer_failure(owner, &work);
        work.status = InteropWindowsSocketWorkPeerFailure;
        goto drive_done;
    }
    if (!owner->closed)
        work.status = InteropWindowsSocketWorkProgress;
    if (owner->closed)
        goto drive_done;
    result = handle_fatal_revents(owner, revents, &work);
    if (result != 0) {
        work.state = owner->state;
        work.terminal_reason = owner->terminal_reason;
        work.wsa_error = owner->terminal_wsa_error;
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        goto drive_done;
    }
    /* A newly surfaced event is the entire caller-visible unit for this
     * drive. Repeated Draining ticks are suppressed by accept_peer_event. */
    if (work.event.kind != InteropPeerEventNone || owner->closed)
        goto drive_done;

    while (owner->state == InteropWindowsSocketActive && owner->rx_length != 0) {
        result = service_carry(owner, &work, frame_out, now_ms, &frame_consumed);
        if (result == InteropWindowsSocketPeerFailure) {
            close_peer_failure(owner, &work);
            work.status = InteropWindowsSocketWorkPeerFailure;
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
    if (owner->state == InteropWindowsSocketActive && owner->rx_length == 0) {
        result = service_carry(owner, &work, frame_out, now_ms, &frame_consumed);
        if (result == InteropWindowsSocketPeerFailure) {
            close_peer_failure(owner, &work);
            work.status = InteropWindowsSocketWorkPeerFailure;
            work.state = owner->state;
            work.terminal_reason = owner->terminal_reason;
            copy_event(&work.diagnostic_event, &owner->terminal_event);
            goto drive_done;
        }
        if (result == InternalWorkExhausted || frame_consumed || owner->closed)
            goto drive_done;
    }

    if (owner->state == InteropWindowsSocketActive ||
        owner->state == InteropWindowsSocketMappingWait) {
        uint32_t revent_read = (uint32_t)(POLLRDNORM | POLLHUP);
        while ((revents & revent_read) != 0 &&
               rx_budget_used < rx_byte_budget &&
               work.syscalls < syscall_budget &&
               owner->rx_length < sizeof(owner->rx_carry)) {
            size_t room;
            size_t amount;
            int received;

            compact_rx(owner);
            room = sizeof(owner->rx_carry) - owner->rx_length;
            amount = rx_byte_budget - rx_budget_used;
            if (amount > room)
                amount = room;
            if (amount == 0)
                break;
            ++work.syscalls;
            received = recv(owner->socket_value, (char *)(owner->rx_carry + owner->rx_length),
                            (int)amount, 0);
            if (received > 0) {
                size_t received_size = (size_t)received;
                owner->rx_length += received_size;
                rx_budget_used += received_size;
                work.rx_bytes += received_size;
                work.status = InteropWindowsSocketWorkProgress;
                if (owner->state == InteropWindowsSocketActive) {
                    result = service_carry(owner, &work, frame_out, now_ms,
                                           &frame_consumed);
                    if (result == InteropWindowsSocketPeerFailure) {
                        close_peer_failure(owner, &work);
                        work.status = InteropWindowsSocketWorkPeerFailure;
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
                terminalize_drive(owner, &work, InteropWindowsSocketTerminalEof, 0);
                goto drive_done;
            }
            if (WSAGetLastError() == WSAEINTR)
                continue;
            if (would_block(WSAGetLastError()))
                break;
            terminalize_drive(owner, &work, InteropWindowsSocketTerminalSocketError, WSAGetLastError());
            goto drive_done;
        }
    }

    if (owner->closed)
        goto drive_done;
    if (owner->state == InteropWindowsSocketDraining) {
        size_t queued = 0;
        if (owner->drain_deadline_set && now_ms >= owner->drain_deadline_ms) {
            terminalize_drive(owner, &work, InteropWindowsSocketTerminalDrainDeadline, 0);
            goto drive_done;
        }
        if (!take_internal_step(&work))
            goto drive_done;
        if (interop_peer_tx_bytes(owner->peer, owner->token, &queued) != InteropPeerSuccess) {
            close_peer_failure(owner, &work);
            work.status = InteropWindowsSocketWorkPeerFailure;
            goto drive_done;
        }
        if (queued == 0) {
            terminalize_drive(owner, &work, InteropWindowsSocketTerminalDrainComplete, 0);
            goto drive_done;
        }
    }

    if (owner->state != InteropWindowsSocketMappingWait &&
        owner->state != InteropWindowsSocketTransportClosed &&
        tx_budget_used < tx_byte_budget && work.syscalls < syscall_budget &&
        (revents & (uint32_t)POLLWRNORM) != 0) {
        while (tx_budget_used < tx_byte_budget && work.syscalls < syscall_budget &&
               owner->state != InteropWindowsSocketTransportClosed) {
            size_t queued = 0;
            size_t remaining;
            size_t amount;
            int sent;
            interop_peer_event_t ack_event;

            if (owner->tx_offset >= owner->tx_length) {
                owner->tx_length = 0;
                owner->tx_offset = 0;
                if (!take_internal_step(&work))
                    break;
                if (interop_peer_tx_bytes(owner->peer, owner->token, &queued) != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropWindowsSocketWorkPeerFailure;
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
                    work.status = InteropWindowsSocketWorkPeerFailure;
                    goto drive_done;
                }
                owner->tx_offset = 0;
            }
            if ((InteropWindowsSocketMaxInternalStepsPerDrive - 1u) - work.internal_steps < 2u)
                break; /* reserve ACK and phase query before any positive send */
            remaining = owner->tx_length - owner->tx_offset;
            amount = tx_byte_budget - tx_budget_used;
            if (amount > remaining)
                amount = remaining;
            if (amount == 0)
                break;
            ++work.syscalls;
            sent = send(owner->socket_value, (const char *)(owner->tx_copy + owner->tx_offset),
                        (int)amount, 0);
            if (sent > 0) {
                size_t sent_size = (size_t)sent;
                work.tx_bytes += sent_size; /* accepted even if ACK then fails */
                tx_budget_used += sent_size;
                owner->tx_offset += sent_size;
                work.status = InteropWindowsSocketWorkProgress;
                if (!take_internal_step(&work))
                    break; /* impossible: two steps were reserved */
                clear_event(&ack_event);
                result = interop_peer_ack_tx(owner->peer, owner->token,
                                             now_ms, sent_size, &ack_event);
                if (result != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropWindowsSocketWorkPeerFailure;
                    work.state = owner->state;
                    work.terminal_reason = owner->terminal_reason;
                    work.wsa_error = owner->terminal_wsa_error;
                    copy_event(&work.diagnostic_event, &owner->terminal_event);
                    goto drive_done;
                }
                if (!take_internal_step(&work))
                    break; /* reserved phase step */
                clear_event(&event);
                result = interop_peer_get_state(owner->peer, owner->token, &event);
                if (result != InteropPeerSuccess) {
                    close_peer_failure(owner, &work);
                    work.status = InteropWindowsSocketWorkPeerFailure;
                    goto drive_done;
                }
                phase = event.phase;
                if (!observe_phase(owner, phase, now_ms, &ack_event)) {
                    close_peer_failure(owner, &work);
                    work.status = InteropWindowsSocketWorkPeerFailure;
                    goto drive_done;
                }
                if (owner->tx_offset == owner->tx_length) {
                    owner->tx_offset = 0;
                    owner->tx_length = 0;
                }
                if (ack_event.kind != InteropPeerEventNone) {
                    if (!accept_event_to_work(owner, &ack_event, &work, now_ms)) {
                        close_peer_failure(owner, &work);
                        work.status = InteropWindowsSocketWorkPeerFailure;
                    }
                    goto drive_done;
                }
                if (owner->state == InteropWindowsSocketDraining &&
                    owner->drain_deadline_set && now_ms >= owner->drain_deadline_ms) {
                    terminalize_drive(owner, &work, InteropWindowsSocketTerminalDrainDeadline, 0);
                    goto drive_done;
                }
                continue;
            }
            if (sent == 0) {
                terminalize_drive(owner, &work, InteropWindowsSocketTerminalSocketError, WSAECONNRESET);
                goto drive_done;
            }
            if (WSAGetLastError() == WSAEINTR)
                continue;
            if (would_block(WSAGetLastError()))
                break;
            terminalize_drive(owner, &work, InteropWindowsSocketTerminalSocketError, WSAGetLastError());
            goto drive_done;
        }
    }

drive_done:
    if (work.status == InteropWindowsSocketWorkIdle &&
        (work.rx_bytes != 0 || work.tx_bytes != 0 || work.event.kind != InteropPeerEventNone))
        work.status = InteropWindowsSocketWorkProgress;
    work.state = owner->state;
    work.terminal_reason = owner->terminal_reason;
    work.wsa_error = owner->terminal_wsa_error;
    work.close_wsa_error = owner->close_wsa_error;
    if (owner->closed) {
        copy_event(&work.diagnostic_event, &owner->terminal_event);
        if (owner->peer_notify_failed)
            work.status = InteropWindowsSocketWorkPeerFailure;
        else if (work.status == InteropWindowsSocketWorkIdle)
            work.status = close_work_status(owner);
    }
    memcpy(out_work, &work, sizeof(work));
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_mapping_result */
int interop_windows_socket_mapping_result(interop_windows_socket_t *owner,
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
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->closed)
        return InteropWindowsSocketClosed;
    result = check_time(owner, now_ms);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->state != InteropWindowsSocketMappingWait || !owner->mapping_callback_pending)
        return InteropWindowsSocketWrongPhase;

    /* Mirror only the Guest callback's documented caller-input failures.
     * Host validation inputs, including epoch/layout mismatch, are evidence
     * for the peer and must be forwarded so it can enter Draining. */
    if (owner->role == InteropPeerGuest) {
        if (layout_version != InteropLayoutVersion ||
            (succeeded == 0) || (epoch == 0))
            return succeeded == 0 && layout_version == InteropLayoutVersion ?
                   InteropWindowsSocketMappingFailed : InteropWindowsSocketInvalidArgument;
    }

    clear_event(&event);
    result = interop_peer_mapping_result(owner->peer, owner->token, now_ms,
                                         epoch, layout_version, succeeded, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, NULL);
        return InteropWindowsSocketPeerFailure;
    }

    commit_time(owner, now_ms);
    owner->mapping_callback_pending = false;
    invalidate_cookie(owner); /* every successful non-drive mapping result */
    if (!observe_phase(owner, event.phase, now_ms, &event)) {
        close_peer_failure(owner, NULL);
        return InteropWindowsSocketPeerFailure;
    }
    if ((event.flags & InteropPeerAbortTransport) != 0)
        apply_abort_event(owner, &event, NULL);
    copy_event(out_event, &event);
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_begin_drain */
int interop_windows_socket_begin_drain(interop_windows_socket_t *owner,
                                     uint64_t token, uint64_t now_ms,
                                     interop_peer_event_t *out_event)
{
    interop_peer_event_t event;
    int result;

    if (owner == NULL || out_event == NULL ||
        !range_valid(out_event, sizeof(*out_event), _Alignof(interop_peer_event_t)) ||
        !output_disjoint_from_owner(owner, out_event, sizeof(*out_event)))
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->closed)
        return InteropWindowsSocketClosed;
    result = check_time(owner, now_ms);
    if (result != InteropWindowsSocketSuccess)
        return result;

    clear_event(&event);
    result = interop_peer_begin_drain(owner->peer, owner->token, now_ms, &event);
    if (result != InteropPeerSuccess) {
        close_peer_failure(owner, NULL);
        return InteropWindowsSocketPeerFailure;
    }

    commit_time(owner, now_ms);
    invalidate_cookie(owner); /* all successful drain operations invalidate */
    if (!observe_phase(owner, event.phase, now_ms, &event)) {
        close_peer_failure(owner, NULL);
        return InteropWindowsSocketPeerFailure;
    }
    if ((event.flags & InteropPeerAbortTransport) != 0)
        apply_abort_event(owner, &event, NULL);
    copy_event(out_event, &event);
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_abort */
int interop_windows_socket_abort(interop_windows_socket_t *owner, uint64_t token,
                               interop_windows_socket_work_t *out_work)
{
    interop_windows_socket_work_t work;
    int result;

    if (owner == NULL || out_work == NULL || !valid_work_output(owner, out_work))
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    init_work(&work, owner);
    invalidate_cookie(owner); /* every successful abort invalidates */
    if (owner->closed) {
        work.status = owner->state == InteropWindowsSocketTransportClosed ?
                      InteropWindowsSocketWorkTransportClosed : close_work_status(owner);
        work.state = owner->state;
        work.wsa_error = owner->terminal_wsa_error;
        memcpy(out_work, &work, sizeof(work));
        return InteropWindowsSocketSuccess;
    }
    close_transport(owner, InteropWindowsSocketTerminalLocalAbort, 0, &work);
    work.state = owner->state;
    work.status = close_work_status(owner);
    work.terminal_reason = owner->terminal_reason;
    work.wsa_error = owner->terminal_wsa_error;
    work.event.kind = InteropPeerEventNone;
    copy_event(&work.diagnostic_event, &owner->terminal_event);
    memcpy(out_work, &work, sizeof(work));
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_destroy */
int interop_windows_socket_destroy(interop_windows_socket_t **owner_slot)
{
    interop_windows_socket_t *owner;
    int peer_result;

    if (owner_slot == NULL ||
        !range_valid(owner_slot, sizeof(*owner_slot), _Alignof(interop_windows_socket_t *)))
        return InteropWindowsSocketInvalidArgument;
    owner = *owner_slot;
    if (owner == NULL)
        return InteropWindowsSocketSuccess;
    if (!on_owner_thread(owner))
        return InteropWindowsSocketWrongThread;
    if (ranges_overlap(owner_slot, sizeof(*owner_slot), owner, sizeof(*owner)))
        return InteropWindowsSocketInvalidArgument;

    if (!owner->closed)
        close_transport(owner, InteropWindowsSocketTerminalDestroyed, 0, NULL);
    if (owner->state == InteropWindowsSocketClosePendingState)
        return InteropWindowsSocketClosePending;
    if (owner->state == InteropWindowsSocketCloseIndeterminateState)
        return InteropWindowsSocketCloseIndeterminate;
    if (owner->peer != NULL && !owner->transport_notified) {
        interop_peer_event_t ignored;
        clear_event(&ignored);
        peer_result = interop_peer_transport_lost(owner->peer, owner->token, &ignored);
        if (peer_result != InteropPeerSuccess)
            return InteropWindowsSocketPeerFailure;
        owner->transport_notified = true;
    }
    if (owner->peer != NULL) {
        peer_result = interop_peer_destroy(&owner->peer);
        if (peer_result != InteropPeerSuccess)
            return InteropWindowsSocketPeerFailure;
    }
    memset(owner, 0, sizeof(*owner));
    free(owner);
    *owner_slot = NULL;
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_retry_close */
int interop_windows_socket_retry_close(interop_windows_socket_t *owner,
                                       uint64_t token)
{
    interop_peer_event_t ignored;
    struct linger linger_value;
    int option_length = (int)sizeof(linger_value);
    uint32_t error_code;
    int result;

    if (owner == NULL)
        return InteropWindowsSocketInvalidArgument;
    result = check_owner_token(owner, token);
    if (result != InteropWindowsSocketSuccess)
        return result;
    if (owner->state != InteropWindowsSocketClosePendingState ||
        owner->close_wsa_error != WSAEWOULDBLOCK)
        return InteropWindowsSocketWrongPhase;
    if (getsockopt(owner->socket_value, SOL_SOCKET, SO_LINGER,
                   (char *)&linger_value, &option_length) == SOCKET_ERROR)
        return InteropWindowsSocketOsError;
    if (option_length != (int)sizeof(linger_value) || linger_value.l_onoff != 0)
        return InteropWindowsSocketClosePending;
    if (closesocket(owner->socket_value) == SOCKET_ERROR) {
        error_code = (uint32_t)WSAGetLastError();
        owner->close_wsa_error = error_code;
        if (owner->terminal_wsa_error == 0)
            owner->terminal_wsa_error = error_code;
        if (error_code != WSAEWOULDBLOCK) {
            owner->state = InteropWindowsSocketCloseIndeterminateState;
            owner->close_indeterminate = true;
            return InteropWindowsSocketCloseIndeterminate;
        }
        return InteropWindowsSocketClosePending;
    }
    owner->socket_value = INVALID_SOCKET;
    owner->state = InteropWindowsSocketTransportClosed;
    owner->close_wsa_error = 0;
    clear_event(&ignored);
    if (interop_peer_transport_lost(owner->peer, owner->token, &ignored) != InteropPeerSuccess) {
        owner->peer_notify_failed = true;
        return InteropWindowsSocketPeerFailure;
    }
    owner->transport_notified = true;
    return InteropWindowsSocketSuccess;
}

/** @copydoc interop_windows_socket_quarantine_handoff */
int interop_windows_socket_quarantine_handoff(
    interop_windows_socket_t **owner_slot, uint64_t token,
    interop_windows_socket_quarantine_t *quarantine_out)
{
    interop_windows_socket_t *owner;
    interop_windows_socket_quarantine_t quarantine;
    int peer_result;

    if (owner_slot == NULL || quarantine_out == NULL ||
        !range_valid(owner_slot, sizeof(*owner_slot), _Alignof(interop_windows_socket_t *)) ||
        !range_valid(quarantine_out, sizeof(*quarantine_out),
                     _Alignof(interop_windows_socket_quarantine_t)) ||
        ranges_overlap(owner_slot, sizeof(*owner_slot), quarantine_out,
                       sizeof(*quarantine_out)))
        return InteropWindowsSocketInvalidArgument;
    owner = *owner_slot;
    if (owner == NULL)
        return InteropWindowsSocketInvalidArgument;
    if (!on_owner_thread(owner))
        return InteropWindowsSocketWrongThread;
    if (ranges_overlap(owner_slot, sizeof(*owner_slot), owner, sizeof(*owner)) ||
        !output_disjoint_from_owner(owner, quarantine_out, sizeof(*quarantine_out)))
        return InteropWindowsSocketInvalidArgument;
    if (token == 0 || token != owner->token)
        return InteropWindowsSocketWrongToken;
    if (owner->state != InteropWindowsSocketClosePendingState &&
        owner->state != InteropWindowsSocketCloseIndeterminateState)
        return InteropWindowsSocketWrongPhase;
    if (quarantine_out->socket_value != 0 || quarantine_out->wsa_error != 0 ||
        quarantine_out->close_classification != 0 || quarantine_out->owner_thread_id != 0 ||
        quarantine_out->reserved_zero != 0 || quarantine_out->connection_token != 0)
        return InteropWindowsSocketInvalidArgument;

    memset(&quarantine, 0, sizeof(quarantine));
    quarantine.socket_value = (uintptr_t)owner->socket_value;
    quarantine.wsa_error = owner->close_wsa_error;
    quarantine.close_classification = owner->state;
    quarantine.owner_thread_id = owner->owner_thread_id;
    quarantine.connection_token = owner->token;
    if (owner->peer != NULL) {
        peer_result = interop_peer_destroy(&owner->peer);
        if (peer_result != InteropPeerSuccess)
            return InteropWindowsSocketPeerFailure;
    }
    memset(owner, 0, sizeof(*owner));
    free(owner);
    *owner_slot = NULL;
    memcpy(quarantine_out, &quarantine, sizeof(quarantine));
    return InteropWindowsSocketSuccess;
}
