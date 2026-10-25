/**
 * @file session_host.c
 * @brief C orchestration for one bounded Linux daemon-to-interop admission.
 *
 * The caller owns daemon_fd and cancel_fd. This module owns only temporary
 * stack state and a successful interop_linux_session_owner_t result.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_host.h"

#include "../daemon/daemon_wire.h"
#include "waddle/interop_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    HostSessionStatusPayloadCapacity = sizeof(waddle_daemon_status_resp_t),
    HostSessionReadyPayloadCapacity = 264,
    HostSessionFrameHeaderBytes = 16,
    HostSessionGuestReadyRequestBytes = 4,
    HostSessionMaxReadyTimeoutMs = 120000,
    HostSessionMaxConnectTimeoutMs = 2000
};

typedef struct host_session_range_t {
    uintptr_t begin;
    uintptr_t end;
} host_session_range_t;

typedef struct host_session_response_t {
    waddle_daemon_header_t header;
    uint8_t payload[HostSessionStatusPayloadCapacity > HostSessionReadyPayloadCapacity ?
        HostSessionStatusPayloadCapacity : HostSessionReadyPayloadCapacity];
} host_session_response_t;

static _Atomic uint32_t host_session_sequence = UINT32_C(0x48530001);

static bool make_range(const void *pointer, size_t bytes, size_t alignment,
                       host_session_range_t *out_range)
{
    uintptr_t begin;
    if (pointer == NULL || out_range == NULL || bytes == 0 || alignment == 0)
        return false;
    begin = (uintptr_t)pointer;
    if (begin % alignment != 0 || bytes > UINTPTR_MAX - begin)
        return false;
    out_range->begin = begin;
    out_range->end = begin + bytes;
    return true;
}

static bool ranges_overlap(host_session_range_t left, host_session_range_t right)
{
    return left.begin < right.end && right.begin < left.end;
}

static bool valid_string(const char *value, size_t capacity)
{
    return value != NULL && capacity != 0 && memchr(value, '\0', capacity) != NULL;
}

static bool valid_capabilities(uint64_t capabilities)
{
    return (capabilities & ~InteropCapabilityMask) == 0;
}

static bool config_valid(const interop_host_session_config_t *config,
                         interop_linux_session_owner_t **out_owner,
                         uint32_t *out_invalidated)
{
    host_session_range_t config_range, output_range, invalidated_range;
    host_session_range_t device_range, version_range;
    const device_info_t *device;

    if (!make_range(config, sizeof(*config), alignof(interop_host_session_config_t), &config_range) ||
        !make_range(out_owner, sizeof(*out_owner), alignof(interop_linux_session_owner_t *), &output_range) ||
        !make_range(out_invalidated, sizeof(*out_invalidated), alignof(uint32_t), &invalidated_range) ||
        !make_range(config->device, sizeof(*config->device), alignof(device_info_t), &device_range) ||
        !make_range(config->agent_version, config->agent_version_bytes, 1, &version_range))
        return false;

    if (ranges_overlap(config_range, output_range) ||
        ranges_overlap(config_range, invalidated_range) ||
        ranges_overlap(output_range, invalidated_range) ||
        ranges_overlap(config_range, device_range) ||
        ranges_overlap(config_range, version_range) ||
        ranges_overlap(device_range, version_range) ||
        ranges_overlap(output_range, device_range) ||
        ranges_overlap(output_range, version_range) ||
        ranges_overlap(invalidated_range, device_range) ||
        ranges_overlap(invalidated_range, version_range) ||
        *out_owner != NULL)
        return false;

    device = config->device;
    if (config->reserved_zero != 0 || config->daemon_fd < 0 ||
        config->cancel_fd < -1 || config->cancel_fd == config->daemon_fd ||
        config->connection_token == 0 || !valid_capabilities(config->local_capabilities) ||
        config->guest_ready_timeout_ms == 0 ||
        config->guest_ready_timeout_ms > HostSessionMaxReadyTimeoutMs ||
        config->drain_timeout_ms == 0 ||
        config->drain_timeout_ms > InteropLinuxSocketMaxDrainTimeoutMs ||
        config->agent_version_bytes < 5 || config->agent_version_bytes > 32 ||
        config->deadline_ms == 0 || device->config_valid != 1 ||
        device->vsock_cid < 3 || device->vsock_cid == UINT32_MAX ||
        device->vsock_port == 0 || device->vsock_port == InteropControlPort ||
        !valid_string(device->name, sizeof(device->name)) ||
        !valid_string(device->state_dir, sizeof(device->state_dir)) ||
        !valid_string(device->socket_path, sizeof(device->socket_path)) ||
        daemon_device_validate_name(device->name) != 0)
        return false;
    return true;
}

static int monotonic_ms(uint64_t *out_ms)
{
    struct timespec now;
    uint64_t seconds;
    uint64_t millis;

    if (out_ms == NULL) { errno = EINVAL; return -1; }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        errno = EOVERFLOW;
        return -1;
    }
    seconds = (uint64_t)now.tv_sec;
    millis = (uint64_t)now.tv_nsec / 1000000u;
    if (seconds > (UINT64_MAX - millis) / 1000u) {
        errno = EOVERFLOW;
        return -1;
    }
    *out_ms = seconds * 1000u + millis;
    return 0;
}

static uint32_t next_sequence(void)
{
    uint32_t sequence = atomic_fetch_add_explicit(&host_session_sequence, 1,
                                                   memory_order_relaxed);
    if (sequence == 0)
        sequence = atomic_fetch_add_explicit(&host_session_sequence, 1,
                                              memory_order_relaxed);
    return sequence;
}

static void store_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void store_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void encode_header(uint8_t bytes[HostSessionFrameHeaderBytes],
                          uint16_t message_type, uint32_t sequence,
                          uint32_t payload_bytes)
{
    memset(bytes, 0, HostSessionFrameHeaderBytes);
    store_le32(bytes, WaddleDaemonMagic);
    store_le16(bytes + 4, WaddleDaemonVersion);
    store_le16(bytes + 6, message_type);
    store_le32(bytes + 8, sequence);
    store_le32(bytes + 12, payload_bytes);
}

static int check_cancel(int cancel_fd)
{
    struct pollfd descriptor;
    int result;
    if (cancel_fd < 0) return 0;
    descriptor = (struct pollfd){.fd = cancel_fd, .events = POLLIN | POLLERR | POLLHUP};
    result = poll(&descriptor, 1, 0);
    if (result < 0) return -1;
    if (result > 0 && descriptor.revents != 0) {
        errno = (descriptor.revents & POLLNVAL) != 0 ? EBADF : ECANCELED;
        return errno == ECANCELED ? 1 : -1;
    }
    return 0;
}

static int check_deadline(uint64_t deadline_ms, uint64_t *out_now_ms)
{
    if (monotonic_ms(out_now_ms) != 0) return -1;
    if (*out_now_ms >= deadline_ms) { errno = ETIMEDOUT; return 1; }
    return 0;
}

static int wait_io(int fd, short events, int cancel_fd, uint64_t deadline_ms)
{
    for (;;) {
        struct pollfd descriptors[2];
        nfds_t count = cancel_fd >= 0 ? 2u : 1u;
        uint64_t now_ms;
        uint64_t remaining;
        int timeout;
        int result;

        int deadline_result = check_deadline(deadline_ms, &now_ms);
        if (deadline_result != 0) return -1;
        remaining = deadline_ms - now_ms;
        timeout = remaining > (uint64_t)INT32_MAX ? INT32_MAX : (int)remaining;
        descriptors[0] = (struct pollfd){.fd = fd, .events = events};
        if (cancel_fd >= 0)
            descriptors[1] = (struct pollfd){.fd = cancel_fd,
                .events = POLLIN | POLLERR | POLLHUP};
        result = poll(descriptors, count, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) continue;
        if (cancel_fd >= 0 && descriptors[1].revents != 0) {
            errno = (descriptors[1].revents & POLLNVAL) != 0 ? EBADF : ECANCELED;
            return -1;
        }
        if (descriptors[0].revents & POLLNVAL) { errno = EBADF; return -1; }
        if (descriptors[0].revents & (events | POLLERR | POLLHUP)) return 0;
    }
}

static int transfer_bytes(int fd, uint8_t *bytes, size_t length, bool writing,
                          int cancel_fd, uint64_t deadline_ms, size_t *out_moved)
{
    size_t moved_total = 0;
    while (moved_total < length) {
        uint64_t now_ms;
        ssize_t moved;
        int deadline_result = check_deadline(deadline_ms, &now_ms);
        if (deadline_result != 0) goto fail;
        if (cancel_fd >= 0) {
            int cancel_result = check_cancel(cancel_fd);
            if (cancel_result != 0) goto fail;
        }
        moved = writing ? send(fd, bytes + moved_total, length - moved_total,
                               MSG_DONTWAIT | MSG_NOSIGNAL) :
                          recv(fd, bytes + moved_total, length - moved_total, MSG_DONTWAIT);
        if (moved > 0) {
            moved_total += (size_t)moved;
            continue;
        }
        if (moved == 0) { errno = writing ? EPIPE : ECONNRESET; goto fail; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) goto fail;
        if (wait_io(fd, writing ? POLLOUT : POLLIN, cancel_fd, deadline_ms) != 0)
            goto fail;
    }
    {
        uint64_t completed_ms;
        int deadline_result = check_deadline(deadline_ms, &completed_ms);
        if (deadline_result != 0) goto fail;
        if (cancel_fd >= 0 && check_cancel(cancel_fd) != 0) goto fail;
    }
    if (out_moved != NULL) *out_moved = moved_total;
    return 0;

fail:
    if (out_moved != NULL) *out_moved = moved_total;
    return -1;
}

static int validate_control_fd(int fd)
{
    struct sockaddr_storage peer_address;
    socklen_t peer_address_bytes = sizeof(peer_address);
    struct ucred credentials;
    socklen_t credentials_bytes = sizeof(credentials);
    int socket_type = 0;
    socklen_t socket_type_bytes = sizeof(socket_type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_bytes) != 0)
        return -1;
    if (socket_type_bytes != sizeof(socket_type) || socket_type != SOCK_STREAM) {
        errno = EPROTOTYPE;
        return -1;
    }
    if (getpeername(fd, (struct sockaddr *)&peer_address, &peer_address_bytes) != 0)
        return -1;
    if (peer_address.ss_family != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_bytes) != 0)
        return -1;
    if (credentials_bytes != sizeof(credentials) || credentials.pid <= 0 ||
        credentials.uid != geteuid()) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int exchange_request(int daemon_fd, int cancel_fd, uint64_t deadline_ms,
                            uint16_t request_type, uint16_t expected_type,
                            const uint8_t *request_payload, size_t request_payload_bytes,
                            size_t expected_response_bytes, uint32_t sequence,
                            host_session_response_t *out_response,
                            uint32_t *out_channel_invalidated)
{
    uint8_t frame[HostSessionFrameHeaderBytes + HostSessionGuestReadyRequestBytes];
    uint8_t header_bytes[HostSessionFrameHeaderBytes];
    size_t sent_bytes = 0;
    size_t received_bytes = 0;
    size_t frame_bytes = HostSessionFrameHeaderBytes + request_payload_bytes;
    size_t payload_capacity = sizeof(out_response->payload);
    waddle_daemon_header_t decoded_header;

    if (request_payload_bytes > HostSessionGuestReadyRequestBytes ||
        frame_bytes > sizeof(frame) || expected_response_bytes > payload_capacity) {
        errno = EINVAL;
        return -1;
    }
    encode_header(frame, request_type, sequence, (uint32_t)request_payload_bytes);
    if (request_payload_bytes != 0)
        memcpy(frame + HostSessionFrameHeaderBytes, request_payload, request_payload_bytes);
    if (transfer_bytes(daemon_fd, frame, frame_bytes, true, cancel_fd,
                       deadline_ms, &sent_bytes) != 0) {
        if (sent_bytes != 0) *out_channel_invalidated = 1;
        return -1;
    }
    *out_channel_invalidated = 1;
    if (transfer_bytes(daemon_fd, header_bytes, sizeof(header_bytes), false,
                       cancel_fd, deadline_ms, &received_bytes) != 0)
        return -1;
    if (daemon_wire_decode_header(header_bytes, sizeof(header_bytes), &decoded_header) != 0 ||
        decoded_header.sequence != sequence ||
        !((decoded_header.msg_type == expected_type &&
           decoded_header.payload_len == expected_response_bytes) ||
          (decoded_header.msg_type == DaemonMsgErrorResp &&
           decoded_header.payload_len == 264)) ||
        decoded_header.payload_len > payload_capacity) {
        errno = EPROTO;
        return -1;
    }
    if (transfer_bytes(daemon_fd, out_response->payload, decoded_header.payload_len,
                       false, cancel_fd, deadline_ms, &received_bytes) != 0)
        return -1;
    if (decoded_header.msg_type != expected_type) {
        uint32_t remote_error = 0;
        if (decoded_header.msg_type == DaemonMsgErrorResp &&
            daemon_wire_decode_error_response(out_response->payload,
                                              decoded_header.payload_len,
                                              &remote_error) == 0) {
            *out_channel_invalidated = 0;
            errno = (int)remote_error;
        } else {
            errno = EPROTO;
        }
        return -1;
    }
    out_response->header = decoded_header;
    *out_channel_invalidated = 0;
    return 0;
}

static interop_host_session_result_t map_lease_result(interop_linux_session_result_t result)
{
    switch (result) {
    case InteropLinuxSessionSuccess: return HostSessionSuccess;
    case InteropLinuxSessionInvalidArgument: return HostSessionInvalidArgument;
    case InteropLinuxSessionOutOfMemory: return HostSessionOutOfMemory;
    case InteropLinuxSessionBusy: return HostSessionBusy;
    case InteropLinuxSessionAvBusy: return HostSessionAvBusy;
    case InteropLinuxSessionCancelled: return HostSessionCancelled;
    case InteropLinuxSessionTimedOut: return HostSessionTimedOut;
    case InteropLinuxSessionConnectError: return HostSessionConnectError;
    case InteropLinuxSessionAttemptConsumed: return HostSessionAttemptConsumed;
    case InteropLinuxSessionPeerCreateError: return HostSessionPeerCreateError;
    case InteropLinuxSessionCloseDiagnostic: return HostSessionCloseDiagnostic;
    case InteropLinuxSessionClockError: return HostSessionClockError;
    case InteropLinuxSessionOsError: return HostSessionOsError;
    default: return HostSessionOsError;
    }
}

static interop_host_session_result_t cleanup_failure(
    interop_linux_session_owner_t **owner_slot,
    interop_linux_session_owner_t **out_owner,
    interop_host_session_result_t result,
    int saved_errno)
{
    if (*owner_slot != NULL) {
        if (interop_linux_session_socket_owner(*owner_slot) != NULL) {
            interop_linux_session_result_t destroy_result =
                interop_linux_session_destroy_peer(*owner_slot);
            if (destroy_result != InteropLinuxSessionSuccess &&
                destroy_result != InteropLinuxSessionCloseDiagnostic) {
                *out_owner = *owner_slot;
                if (saved_errno != 0) errno = saved_errno;
                return HostSessionCleanupRequired;
            }
            if (destroy_result == InteropLinuxSessionCloseDiagnostic && saved_errno == 0)
                saved_errno = errno == 0 ? EIO : errno;
        }
        interop_linux_session_result_t release_result =
            interop_linux_session_release(owner_slot);
        if (release_result != InteropLinuxSessionSuccess &&
            release_result != InteropLinuxSessionCloseDiagnostic) {
            if (*owner_slot != NULL) *out_owner = *owner_slot;
            if (saved_errno != 0) errno = saved_errno;
            return HostSessionCleanupRequired;
        }
    }
    if (saved_errno != 0) errno = saved_errno;
    return result;
}

interop_host_session_result_t interop_host_session_admit(
    const interop_host_session_config_t *config,
    interop_linux_session_owner_t **out_owner,
    uint32_t *out_control_channel_invalidated)
{
    interop_linux_session_config_t lease_config;
    interop_linux_session_owner_t *owner = NULL;
    host_session_response_t response;
    waddle_daemon_status_resp_t status;
    waddle_daemon_guest_ready_resp_t ready;
    interop_host_session_result_t host_result = HostSessionSuccess;
    interop_linux_session_result_t lease_result;
    uint64_t now_ms;
    uint32_t remaining_ms;
    uint32_t ready_timeout_ms;
    uint32_t sequence;
    uint32_t invalidated = 0;
    int saved_errno = 0;

    if (config == NULL || out_owner == NULL || out_control_channel_invalidated == NULL)
        return HostSessionInvalidArgument;
    if (!config_valid(config, out_owner, out_control_channel_invalidated))
        return HostSessionInvalidArgument;
    *out_control_channel_invalidated = 0;
    if (validate_control_fd(config->daemon_fd) != 0)
        return HostSessionDaemonError;
    if (config->cancel_fd >= 0) {
        int cancel_result = check_cancel(config->cancel_fd);
        if (cancel_result != 0)
            return cancel_result > 0 ? HostSessionCancelled : HostSessionOsError;
    }
    int deadline_result = check_deadline(config->deadline_ms, &now_ms);
    if (deadline_result != 0)
        return deadline_result > 0 ? HostSessionTimedOut : HostSessionClockError;

    memset(&lease_config, 0, sizeof(lease_config));
    lease_config.device = config->device;
    lease_config.agent_version = config->agent_version;
    lease_config.agent_version_bytes = config->agent_version_bytes;
    lease_config.connection_token = config->connection_token;
    lease_config.local_capabilities = config->local_capabilities;
    lease_config.drain_timeout_ms = config->drain_timeout_ms;
    lease_result = interop_linux_session_acquire(&lease_config, &owner);
    if (lease_result != InteropLinuxSessionSuccess)
        return map_lease_result(lease_result);

    if (config->cancel_fd >= 0) {
        int cancel_result = check_cancel(config->cancel_fd);
        if (cancel_result != 0) {
            host_result = cancel_result > 0 ? HostSessionCancelled : HostSessionOsError;
            saved_errno = errno;
            return cleanup_failure(&owner, out_owner, host_result, saved_errno);
        }
    }
    deadline_result = check_deadline(config->deadline_ms, &now_ms);
    if (deadline_result != 0) {
        host_result = deadline_result > 0 ? HostSessionTimedOut : HostSessionClockError;
        saved_errno = errno;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }

    sequence = next_sequence();
    memset(&response, 0, sizeof(response));
    if (exchange_request(config->daemon_fd, config->cancel_fd, config->deadline_ms,
                         DaemonMsgStatusReq, DaemonMsgStatusResp, NULL, 0,
                         sizeof(status), sequence, &response,
                         &invalidated) != 0) {
        *out_control_channel_invalidated = invalidated;
        saved_errno = errno;
        host_result = errno == ECANCELED ? HostSessionCancelled :
            errno == ETIMEDOUT ? HostSessionTimedOut : HostSessionDaemonError;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    if (daemon_wire_decode_status_response(response.payload,
            response.header.payload_len, &status) != 0) {
        *out_control_channel_invalidated = 1;
        saved_errno = EPROTO;
        return cleanup_failure(&owner, out_owner, HostSessionDaemonError, saved_errno);
    }
    if (status.subsystem_state != SubsystemStateRunning) {
        return cleanup_failure(&owner, out_owner, HostSessionNotRunning, 0);
    }
    if (status.vsock_cid != config->device->vsock_cid ||
        status.vsock_port != config->device->vsock_port) {
        return cleanup_failure(&owner, out_owner, HostSessionDaemonError, EPROTO);
    }

    deadline_result = check_deadline(config->deadline_ms, &now_ms);
    if (deadline_result != 0) {
        host_result = deadline_result > 0 ? HostSessionTimedOut : HostSessionClockError;
        saved_errno = errno;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    remaining_ms = (uint32_t)((config->deadline_ms - now_ms) > UINT32_MAX ?
        UINT32_MAX : (config->deadline_ms - now_ms));
    if (remaining_ms == 0) return cleanup_failure(&owner, out_owner, HostSessionTimedOut, ETIMEDOUT);
    ready_timeout_ms = config->guest_ready_timeout_ms < remaining_ms ?
        config->guest_ready_timeout_ms : remaining_ms;

    uint8_t ready_payload[HostSessionGuestReadyRequestBytes];
    store_le32(ready_payload, ready_timeout_ms);
    sequence = next_sequence();
    memset(&response, 0, sizeof(response));
    invalidated = 0;
    if (exchange_request(config->daemon_fd, config->cancel_fd, config->deadline_ms,
                         DaemonMsgGuestReadyReq, DaemonMsgGuestReadyResp,
                         ready_payload, sizeof(ready_payload), sizeof(ready),
                         sequence, &response, &invalidated) != 0) {
        *out_control_channel_invalidated = invalidated;
        saved_errno = errno;
        host_result = errno == ECANCELED ? HostSessionCancelled :
            errno == ETIMEDOUT ? HostSessionTimedOut : HostSessionDaemonError;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    if (daemon_wire_decode_ready_response(response.payload,
            response.header.payload_len, &ready) != 0) {
        *out_control_channel_invalidated = 1;
        return cleanup_failure(&owner, out_owner, HostSessionDaemonError, EPROTO);
    }
    if (ready.cid != config->device->vsock_cid) {
        return cleanup_failure(&owner, out_owner, HostSessionDaemonError, EPROTO);
    }
    if (ready.status != 0)
        return cleanup_failure(&owner, out_owner, HostSessionGuestUnavailable, -ready.status);

    if (config->cancel_fd >= 0) {
        int cancel_result = check_cancel(config->cancel_fd);
        if (cancel_result != 0) {
            host_result = cancel_result > 0 ? HostSessionCancelled : HostSessionOsError;
            saved_errno = errno;
            return cleanup_failure(&owner, out_owner, host_result, saved_errno);
        }
    }
    deadline_result = check_deadline(config->deadline_ms, &now_ms);
    if (deadline_result != 0) {
        host_result = deadline_result > 0 ? HostSessionTimedOut : HostSessionClockError;
        saved_errno = errno;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    uint64_t remaining = config->deadline_ms - now_ms;
    uint32_t connect_timeout_ms = remaining > HostSessionMaxConnectTimeoutMs ?
        HostSessionMaxConnectTimeoutMs : (uint32_t)remaining;
    if (connect_timeout_ms == 0)
        return cleanup_failure(&owner, out_owner, HostSessionTimedOut, ETIMEDOUT);
    lease_result = interop_linux_session_connect_peer(owner, connect_timeout_ms,
                                                      config->cancel_fd);
    if (lease_result != InteropLinuxSessionSuccess) {
        host_result = map_lease_result(lease_result);
        saved_errno = errno;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    deadline_result = check_deadline(config->deadline_ms, &now_ms);
    if (deadline_result != 0) {
        host_result = deadline_result > 0 ? HostSessionTimedOut : HostSessionClockError;
        saved_errno = errno;
        return cleanup_failure(&owner, out_owner, host_result, saved_errno);
    }
    if (config->cancel_fd >= 0) {
        int cancel_result = check_cancel(config->cancel_fd);
        if (cancel_result != 0) {
            host_result = cancel_result > 0 ? HostSessionCancelled : HostSessionOsError;
            saved_errno = errno;
            return cleanup_failure(&owner, out_owner, host_result, saved_errno);
        }
    }
    *out_owner = owner;
    return HostSessionSuccess;
}
