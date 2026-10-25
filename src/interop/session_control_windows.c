/** @file session_control_windows.c @brief Bounded Windows guest control session. */
#include "session_control_windows.h"
#include "windows_socket_owner.h"
#include "interop_mapping_windows.h"
#include "interop_mapping_init.h"
#include "interop_layout.h"
#include "interop_native_atomic.h"
#include "waddle/interop_protocol.h"
#if !defined(InteropGuestSessionTestApi)
#include <bcrypt.h>
#endif
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(InteropGuestSessionTestApi)
#include "session_control_windows_api_double.h"
#define SessionNullHandle ((HANDLE)0)
#define SessionInvalidHandle ((HANDLE)(intptr_t)-1)
#ifndef WaddleSessionAgentVersion
#define WaddleSessionAgentVersion "0.0.0"
#endif
#define session_wsa_startup interop_guest_test_wsa_startup
#define session_wsa_cleanup interop_guest_test_wsa_cleanup
#define session_open_family interop_guest_test_open_family
#define session_query_family interop_guest_test_query_family
#define session_close_handle interop_guest_test_close_handle
#define session_create_socket interop_guest_test_create_socket
#define session_set_noninherit interop_guest_test_set_noninherit
#define session_set_nonblocking interop_guest_test_set_nonblocking
#define session_bind interop_guest_test_bind
#define session_listen interop_guest_test_listen
#define session_poll interop_guest_test_poll
#define session_accept interop_guest_test_accept
#define session_peer_address interop_guest_test_peer_address
#define session_close_socket interop_guest_test_close_socket
#define session_wait_stop interop_guest_test_wait_stop
#define session_now interop_guest_test_now
#define session_thread_id interop_guest_test_thread_id
#define session_random interop_guest_test_random
#define session_last_wsa_error interop_guest_test_last_wsa_error
#define session_last_win32_error interop_guest_test_last_win32_error
#define session_mapping_create interop_guest_test_mapping_create
#define session_mapping_open interop_guest_test_mapping_open
#define session_mapping_close interop_guest_test_mapping_close
#define session_mapping_destroy interop_guest_test_mapping_destroy
#define session_mapping_initialize interop_guest_test_mapping_initialize
#define session_socket_create interop_guest_test_socket_create
#define session_socket_destroy interop_guest_test_socket_destroy
#define session_socket_retry_close interop_guest_test_socket_retry_close
#define session_socket_poll_events interop_guest_test_socket_poll_events
#define session_socket_drive interop_guest_test_socket_drive
#define session_socket_mapping_result interop_guest_test_socket_mapping_result
#define session_socket_begin_drain interop_guest_test_socket_begin_drain
#define session_socket_quarantine_handoff interop_guest_test_socket_quarantine_handoff
#define session_heartbeat_load interop_guest_test_heartbeat_load
#define session_heartbeat_store_next interop_guest_test_heartbeat_store_next
#define session_allocate interop_guest_test_calloc
#else
#define SessionNullHandle NULL
#define SessionInvalidHandle INVALID_HANDLE_VALUE
#ifndef WaddleSessionAgentVersion
#define WaddleSessionAgentVersion "0.0.0"
#endif
#define session_wsa_startup(version, data) WSAStartup((version), (data))
#define session_wsa_cleanup() WSACleanup()
#define session_open_family() CreateFileW(L"\\\\.\\Viosock", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
#define session_query_family(handle, family, returned) DeviceIoControl((handle), InteropGuestSessionViosockFamilyIoctl, NULL, 0, (family), sizeof(*(family)), (returned), NULL)
#define session_close_handle(handle) CloseHandle(handle)
#define session_create_socket(family) WSASocketW((family), SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT)
#define session_set_noninherit(socket_value) SetHandleInformation((HANDLE)(uintptr_t)(socket_value), HANDLE_FLAG_INHERIT, 0)
#define session_set_nonblocking(socket_value) session_native_set_nonblocking((socket_value))
#define session_bind(socket_value, address, length) bind((socket_value), (const struct sockaddr *)(address), (int)(length))
#define session_listen(socket_value) listen((socket_value), 1)
#define session_poll(descriptor, count, timeout) WSAPoll((descriptor), (count), (timeout))
#define session_accept(socket_value) accept((socket_value), NULL, NULL)
#define session_peer_address(socket_value, address, length) getpeername((socket_value), (struct sockaddr *)(address), (length))
#define session_close_socket(socket_value) closesocket(socket_value)
#define session_wait_stop(event, timeout) WaitForSingleObject((event), (timeout))
#define session_now() GetTickCount64()
#define session_thread_id() GetCurrentThreadId()
#define session_random(output, length) BCryptGenRandom(NULL, (PUCHAR)(output), (ULONG)(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG)
#define session_last_wsa_error() WSAGetLastError()
#define session_last_win32_error() GetLastError()
#define session_mapping_create(owner_out) interop_mapping_windows_create((owner_out))
#define session_mapping_open(owner, index, view_out) interop_mapping_windows_open((owner), (index), (view_out))
#define session_mapping_close(owner, proof) interop_mapping_windows_close((owner), (proof))
#define session_mapping_destroy(owner_inout) interop_mapping_windows_destroy((owner_inout))
#define session_mapping_initialize(base, length, epoch, proof) interop_mapping_initialize((base), (length), (epoch), (proof))
#define session_socket_create interop_windows_socket_create
#define session_socket_destroy interop_windows_socket_destroy
#define session_socket_retry_close interop_windows_socket_retry_close
#define session_socket_poll_events interop_windows_socket_poll_events
#define session_socket_drive interop_windows_socket_drive
#define session_socket_mapping_result interop_windows_socket_mapping_result
#define session_socket_begin_drain interop_windows_socket_begin_drain
#define session_socket_quarantine_handoff interop_windows_socket_quarantine_handoff
#define session_heartbeat_load interop_heartbeat_load
#define session_heartbeat_store_next interop_heartbeat_store_next
#define session_allocate calloc
#endif

#if !defined(InteropGuestSessionTestApi)
static int session_native_set_nonblocking(SOCKET socket_value) {
    u_long nonblocking = 1;
    return ioctlsocket(socket_value, FIONBIO, &nonblocking) == 0;
}
#endif

typedef struct interop_guest_vsock_address_t {
    uint16_t family;
    uint16_t reserved;
    uint32_t port;
    uint32_t cid;
} interop_guest_vsock_address_t;
_Static_assert(sizeof(interop_guest_vsock_address_t) == 12, "Viosock address ABI");

typedef struct interop_guest_session_owner_t {
    DWORD owner_thread;
    uint64_t token;
    uint64_t epoch;
    uint64_t accepted_at;
    uint64_t run_deadline;
    uint64_t next_heartbeat;
    uint64_t last_guest_heartbeat;
    uint32_t operational_result;
    uint32_t last_win32_error;
    uint32_t last_wsa_error;
    uint32_t last_ntstatus;
    uint32_t mapping_status;
    uint32_t cleanup_error_count;
    uint32_t published;
    uint32_t cleanup_retry_mode;
    uint32_t socket_close_indeterminate;
    uint32_t socket_close_pending;
    uint32_t socket_destroy_attempted;
    uint32_t socket_destroy_retryable;
    uint32_t listener_close_pending;
    uint32_t accepted_close_pending;
    uint32_t listener_close_wsa_error;
    uint32_t accepted_close_wsa_error;
    uint32_t listener_close_indeterminate;
    uint32_t accepted_close_indeterminate;
    uint32_t handle_close_indeterminate;
    uint32_t winsock_cleanup_pending;
    uint32_t has_last_now;
    uint32_t clock_error;
    uint32_t winsock_started;
    uint64_t last_now;
    HANDLE family_handle;
    SOCKET listener;
    SOCKET accepted_socket;
    interop_windows_socket_t *socket_owner;
    interop_mapping_windows_t *mapping_owner;
    interop_mapping_windows_view_t mapping_view;
} interop_guest_session_owner_t;

static void record_operational(interop_guest_session_owner_t *owner, uint32_t value) {
    if (owner->operational_result == InteropGuestSessionSuccess) { owner->operational_result = value; }
}
static void record_wsa(interop_guest_session_owner_t *owner) {
    owner->last_wsa_error = (uint32_t)session_last_wsa_error();
}
static void record_wsa_code(interop_guest_session_owner_t *owner, int error) {
    owner->last_wsa_error = (uint32_t)error;
}
static void record_win32(interop_guest_session_owner_t *owner) {
    if (owner->last_win32_error == 0) { owner->last_win32_error = (uint32_t)session_last_win32_error(); }
}
static void record_win32_code(interop_guest_session_owner_t *owner, DWORD error) {
    if (owner->last_win32_error == 0) { owner->last_win32_error = error; }
}
static uint64_t current_now(interop_guest_session_owner_t *owner) {
    uint64_t now = session_now();
    if (now == UINT64_MAX || (owner->has_last_now != 0 && now < owner->last_now)) {
        record_win32(owner);
        owner->clock_error = 1;
        record_operational(owner, InteropGuestSessionOsError);
        return UINT64_MAX;
    }
    owner->has_last_now = 1;
    owner->last_now = now;
    return now;
}
static int is_owner_thread(const interop_guest_session_owner_t *owner) {
    return owner != NULL && owner->owner_thread == session_thread_id();
}
static int local_range_valid(const void *pointer, size_t length, size_t alignment) {
    uintptr_t start = (uintptr_t)pointer;
    if (pointer == NULL || length == 0 || alignment == 0 || start % alignment != 0 || start > UINTPTR_MAX - length) { return 0; }
    return 1;
}
static int local_ranges_overlap(const void *left, size_t left_length, const void *right, size_t right_length) {
    uintptr_t left_start = (uintptr_t)left, right_start = (uintptr_t)right;
    if (left_length == 0 || right_length == 0 || left_start > UINTPTR_MAX - left_length || right_start > UINTPTR_MAX - right_length) { return 1; }
    return left_start < right_start + right_length && right_start < left_start + left_length;
}
static uint32_t timeout_to(uint64_t now, uint64_t deadline);
#if defined(InteropGuestSessionTestApi)
/** @brief Exercise pure pointer-range and timer helpers in the private fixture. */
int interop_guest_test_session_helpers(const void *pointer, size_t length, size_t alignment,
    const void *left, size_t left_length, const void *right, size_t right_length,
    uint64_t now, uint64_t deadline, uint32_t *range_out, uint32_t *overlap_out,
    uint32_t *timeout_out) {
    if (range_out == NULL || overlap_out == NULL || timeout_out == NULL) { return 0; }
    *range_out = (uint32_t)local_range_valid(pointer, length, alignment);
    *overlap_out = (uint32_t)local_ranges_overlap(left, left_length, right, right_length);
    *timeout_out = timeout_to(now, deadline);
    return 1;
}
/** @brief Exercise the NULL-owner guard in the private fixture. */
int interop_guest_test_is_owner_thread_null(void) { return is_owner_thread(NULL); }
#endif
static int wait_stop(interop_guest_session_owner_t *owner, HANDLE event) {
    if (event == SessionNullHandle) { return 0; }
    DWORD result = session_wait_stop(event, 0);
    if (result == WAIT_OBJECT_0) { return 1; }
    if (result == WAIT_TIMEOUT) { return 0; }
    record_win32(owner);
    owner->clock_error = 1;
    record_operational(owner, InteropGuestSessionOsError);
    return -1;
}
static uint32_t timeout_to(uint64_t now, uint64_t deadline) {
    if (now >= deadline) { return 0; }
    uint64_t delta = deadline - now;
    return (uint32_t)(delta > InteropGuestSessionPollSliceMs ? InteropGuestSessionPollSliceMs : delta);
}
static void close_socket_slot(interop_guest_session_owner_t *owner, SOCKET *slot,
                              uint32_t *pending, uint32_t *indeterminate, uint32_t *close_error) {
    if (*slot == INVALID_SOCKET) { return; }
    if (*indeterminate != 0) { return; }
    if (session_close_socket(*slot) == 0) { *slot = INVALID_SOCKET; *pending = 0; *indeterminate = 0; *close_error = 0; }
    else {
        record_wsa(owner);
        *close_error = owner->last_wsa_error;
        *pending = owner->last_wsa_error == WSAEWOULDBLOCK ? 1u : 0u;
        *indeterminate = *pending == 0 ? 1u : 0u;
        owner->cleanup_error_count++;
    }
}
static void close_handle_slot(interop_guest_session_owner_t *owner) {
    if (owner->family_handle == SessionNullHandle || owner->family_handle == SessionInvalidHandle) { return; }
    if (owner->handle_close_indeterminate != 0 && owner->cleanup_retry_mode == 0) { return; }
    if (session_close_handle(owner->family_handle)) { owner->family_handle = SessionNullHandle; owner->handle_close_indeterminate = 0; }
    else { record_win32(owner); owner->handle_close_indeterminate = 1; owner->cleanup_error_count++; }
}
static void cleanup(interop_guest_session_owner_t *owner, uint32_t detached) {
    close_socket_slot(owner, &owner->listener, &owner->listener_close_pending,
                      &owner->listener_close_indeterminate, &owner->listener_close_wsa_error);
    close_socket_slot(owner, &owner->accepted_socket, &owner->accepted_close_pending,
                      &owner->accepted_close_indeterminate, &owner->accepted_close_wsa_error);
    if (owner->socket_owner != NULL) {
        int result;
        if (owner->socket_destroy_attempted == 0) {
            owner->socket_destroy_attempted = 1;
            result = session_socket_destroy(&owner->socket_owner);
        } else if (owner->socket_destroy_retryable != 0 && owner->cleanup_retry_mode != 0) {
            result = session_socket_destroy(&owner->socket_owner);
        } else if (owner->socket_close_pending != 0 && owner->cleanup_retry_mode != 0) {
            result = session_socket_retry_close(owner->socket_owner, owner->token);
            if (result == InteropWindowsSocketSuccess) {
                owner->socket_close_pending = 0;
                result = session_socket_destroy(&owner->socket_owner);
            }
        } else {
            result = owner->socket_close_pending != 0 ? InteropWindowsSocketClosePending :
                owner->socket_close_indeterminate != 0 ? InteropWindowsSocketCloseIndeterminate :
                InteropWindowsSocketPeerFailure;
        }
        if (result != InteropWindowsSocketSuccess) {
            owner->socket_close_pending = result == InteropWindowsSocketClosePending;
            owner->socket_close_indeterminate = result == InteropWindowsSocketCloseIndeterminate;
            owner->socket_destroy_retryable = result != InteropWindowsSocketClosePending &&
                result != InteropWindowsSocketCloseIndeterminate;
            owner->cleanup_error_count++;
        } else {
            owner->socket_close_pending = 0; owner->socket_close_indeterminate = 0;
            owner->socket_destroy_retryable = 0;
        }
    }
    if (owner->mapping_owner != NULL) {
        interop_mapping_windows_status_t status = (interop_mapping_windows_status_t)session_mapping_close(owner->mapping_owner, detached);
        owner->mapping_status = (uint32_t)status;
        if (status == InteropMappingWindowsOk) {
            interop_mapping_windows_status_t destroy_status =
                (interop_mapping_windows_status_t)session_mapping_destroy(&owner->mapping_owner);
            if (destroy_status != InteropMappingWindowsOk) { owner->cleanup_error_count++; }
        } else { owner->cleanup_error_count++; }
    }
    close_handle_slot(owner);
    if (owner->winsock_started && owner->listener == INVALID_SOCKET && owner->accepted_socket == INVALID_SOCKET &&
        owner->socket_owner == NULL && owner->family_handle == SessionNullHandle &&
        (owner->winsock_cleanup_pending == 0 || owner->cleanup_retry_mode != 0)) {
        if (session_wsa_cleanup() == 0) { owner->winsock_started = 0; owner->winsock_cleanup_pending = 0; }
        else { record_wsa(owner); owner->winsock_cleanup_pending = 1; owner->cleanup_error_count++; }
    }
    owner->cleanup_retry_mode = 0;
}
static int resources_remain(const interop_guest_session_owner_t *owner) {
    return owner->winsock_started || owner->family_handle != SessionNullHandle || owner->listener != INVALID_SOCKET ||
        owner->accepted_socket != INVALID_SOCKET || owner->socket_owner != NULL || owner->mapping_owner != NULL;
}
static void publish_state(const interop_guest_session_owner_t *owner, interop_guest_session_state_t *out) {
    interop_guest_session_state_t value = {0};
    value.winsock_started = owner->winsock_started;
    value.family_handle_owned = owner->family_handle != SessionNullHandle && owner->family_handle != SessionInvalidHandle;
    value.listener_owned = owner->listener != INVALID_SOCKET;
    value.accepted_socket_owned = owner->accepted_socket != INVALID_SOCKET;
    value.socket_owner_owned = owner->socket_owner != NULL;
    value.mapping_owner_owned = owner->mapping_owner != NULL;
    value.mapping_view_published = owner->published;
    value.operational_result = owner->operational_result;
    value.last_win32_error = owner->last_win32_error;
    value.last_wsa_error = owner->last_wsa_error;
    value.last_ntstatus = owner->last_ntstatus;
    value.mapping_status = owner->mapping_status;
    value.cleanup_error_count = owner->cleanup_error_count;
    *out = value;
}
static int accept_connection(interop_guest_session_owner_t *owner, uint16_t family,
                             const interop_guest_session_config_t *config) {
    interop_guest_vsock_address_t address = {family, 0, 5002u, UINT32_MAX};
    owner->listener = session_create_socket((int)family);
    if (owner->listener == INVALID_SOCKET) { record_wsa(owner); return InteropGuestSessionOsError; }
    if (!session_set_noninherit((SOCKET)owner->listener)) { record_win32(owner); return InteropGuestSessionOsError; }
    if (!session_set_nonblocking(owner->listener)) { record_wsa(owner); return InteropGuestSessionOsError; }
    if (session_bind(owner->listener, &address, sizeof(address)) != 0 || session_listen(owner->listener) != 0) {
        record_wsa(owner); return InteropGuestSessionOsError;
    }
    uint64_t start = current_now(owner);
    if (start == UINT64_MAX || start > UINT64_MAX - InteropGuestSessionAcceptTimeoutMs) { return InteropGuestSessionOsError; }
    uint64_t deadline = start + InteropGuestSessionAcceptTimeoutMs;
    while (1) {
        uint64_t now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        if (now >= deadline) { return InteropGuestSessionTimedOut; }
        int stop = wait_stop(owner, config->stop_event);
        if (stop < 0) { return InteropGuestSessionOsError; }
        if (stop > 0) { return InteropGuestSessionStopped; }
        WSAPOLLFD poll_descriptor;
        poll_descriptor.fd = owner->listener;
        poll_descriptor.events = POLLRDNORM;
        poll_descriptor.revents = 0;
        now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        int ready = session_poll(&poll_descriptor, 1, (int)timeout_to(now, deadline));
        if (ready < 0) { record_wsa(owner); return InteropGuestSessionOsError; }
        stop = wait_stop(owner, config->stop_event);
        if (stop < 0) { return InteropGuestSessionOsError; }
        if (stop > 0) { return InteropGuestSessionStopped; }
        now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        if (now >= deadline) { return InteropGuestSessionTimedOut; }
        if (ready == 0) { continue; }
        if ((poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { return InteropGuestSessionOsError; }
        if ((poll_descriptor.revents & POLLRDNORM) == 0) { continue; }
        owner->accepted_socket = session_accept(owner->listener);
        if (owner->accepted_socket == INVALID_SOCKET) { record_wsa(owner); return InteropGuestSessionOsError; }
        if (!session_set_noninherit(owner->accepted_socket)) { record_win32(owner); return InteropGuestSessionOsError; }
        if (!session_set_nonblocking(owner->accepted_socket)) { record_wsa(owner); return InteropGuestSessionOsError; }
        interop_guest_vsock_address_t remote = {0};
        int remote_length = sizeof(remote);
        if (session_peer_address(owner->accepted_socket, &remote, &remote_length) != 0) { record_wsa(owner); return InteropGuestSessionOsError; }
        if (remote_length != sizeof(remote) || remote.family != family || remote.reserved != 0 || remote.cid != InteropGuestSessionHostCid) {
            return InteropGuestSessionRejectedPeer;
        }
        interop_windows_socket_config_t socket_config = {0};
        static const uint8_t version[] = WaddleSessionAgentVersion;
        socket_config.peer_config.role = InteropPeerGuest;
        socket_config.peer_config.connection_token = owner->token;
        socket_config.peer_config.accepted_at_ms = current_now(owner);
        if (socket_config.peer_config.accepted_at_ms == UINT64_MAX) {
            return InteropGuestSessionOsError;
        }
        socket_config.peer_config.agent_version = version;
        socket_config.peer_config.agent_version_bytes = sizeof(version) - 1;
        socket_config.peer_config.local_capabilities = 0;
        socket_config.drain_timeout_ms = InteropGuestSessionDrainTimeoutMs;
        int create_result = session_socket_create(owner->accepted_socket, &socket_config, &owner->socket_owner);
        if (create_result != InteropWindowsSocketSuccess) { record_wsa(owner); return InteropGuestSessionOsError; }
        owner->accepted_socket = INVALID_SOCKET;
        owner->accepted_at = socket_config.peer_config.accepted_at_ms;
        if (owner->accepted_at > UINT64_MAX - InteropGuestSessionRunTimeoutMs) { return InteropGuestSessionOsError; }
        owner->run_deadline = owner->accepted_at + InteropGuestSessionRunTimeoutMs;
        return InteropGuestSessionSuccess;
    }
    return InteropGuestSessionTimedOut;
}
static int initialize_mapping(interop_guest_session_owner_t *owner, uint32_t device_index) {
    interop_mapping_windows_status_t status = (interop_mapping_windows_status_t)session_mapping_create(&owner->mapping_owner);
    if (status != InteropMappingWindowsOk) { owner->mapping_status = status; return InteropGuestSessionMappingFailed; }
    status = (interop_mapping_windows_status_t)session_mapping_open(owner->mapping_owner, device_index, &owner->mapping_view);
    owner->mapping_status = status;
    if (status != InteropMappingWindowsOk) { return InteropGuestSessionMappingFailed; }
    owner->published = 1;
    int32_t random_status = (int32_t)session_random(&owner->epoch, sizeof(owner->epoch));
    if (random_status != 0) {
        owner->epoch = 0;
        owner->last_ntstatus = (uint32_t)random_status;
        return InteropGuestSessionOsError;
    }
    if (owner->epoch == 0) {
        record_win32_code(owner, ERROR_INVALID_DATA);
        return InteropGuestSessionOsError;
    }
    interop_mapping_init_status_t init_status = (interop_mapping_init_status_t)session_mapping_initialize(
        owner->mapping_view.mapping_base, owner->mapping_view.mapping_length, owner->epoch, 1);
    if (init_status != InteropMappingInitStatusOk) { return InteropGuestSessionMappingFailed; }
    interop_peer_event_t event = {0};
    uint64_t now = current_now(owner);
    if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
    int map_result = session_socket_mapping_result(owner->socket_owner, owner->token,
        now, owner->epoch, InteropLayoutVersion, 1, &event);
    if (map_result != InteropWindowsSocketSuccess) { return InteropGuestSessionPeerFailed; }
    return InteropGuestSessionSuccess;
}
static int validate_header(interop_guest_session_owner_t *owner) {
    uint8_t snapshot_bytes[4096];
    uint8_t *base = (uint8_t *)(uintptr_t)owner->mapping_view.mapping_base;
    memcpy(snapshot_bytes, base, 56);
    memcpy(snapshot_bytes + 72, base + 72, sizeof(snapshot_bytes) - 72);
    interop_shm_header_t *header = (interop_shm_header_t *)base;
    uint64_t guest = 0, host = 0;
    if (session_heartbeat_load(header, owner->mapping_view.mapping_base, owner->mapping_view.mapping_length, InteropGuestHeartbeat, &guest) != InteropAtomicStatusOk ||
        session_heartbeat_load(header, owner->mapping_view.mapping_base, owner->mapping_view.mapping_length, InteropHostHeartbeat, &host) != InteropAtomicStatusOk) {
        return InteropGuestSessionMappingFailed;
    }
    memcpy(snapshot_bytes + 56, &guest, sizeof(guest));
    memcpy(snapshot_bytes + 64, &host, sizeof(host));
    interop_shm_header_snapshot_t parsed;
    if (interop_layout_validate_shm_header(snapshot_bytes, sizeof(snapshot_bytes), owner->mapping_view.mapping_base,
            owner->mapping_view.mapping_length, owner->epoch, &parsed) != InteropLayoutStatusOk) {
        return InteropGuestSessionMappingFailed;
    }
    owner->last_guest_heartbeat = guest;
    uint64_t now = current_now(owner);
    if (now == UINT64_MAX || now > UINT64_MAX - InteropGuestSessionHeartbeatPeriodMs) { return InteropGuestSessionOsError; }
    owner->next_heartbeat = now + InteropGuestSessionHeartbeatPeriodMs;
    return InteropGuestSessionSuccess;
}
static int run_loop(interop_guest_session_owner_t *owner, HANDLE stop_event) {
    uint8_t frame[InteropWindowsSocketFrameCapacity];
    while (1) {
        uint64_t now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        if (now >= owner->run_deadline) { return InteropGuestSessionTimedOut; }
        int stop = wait_stop(owner, stop_event);
        if (stop < 0) { return InteropGuestSessionOsError; }
        if (stop > 0) { return InteropGuestSessionStopped; }
        interop_windows_socket_poll_t socket_poll;
        int result = session_socket_poll_events(owner->socket_owner, owner->token, now, &socket_poll);
        if (result != InteropWindowsSocketSuccess) { return InteropGuestSessionPeerFailed; }
        WSAPOLLFD descriptor;
        descriptor.fd = (SOCKET)socket_poll.socket_value;
        descriptor.events = (short)socket_poll.events;
        descriptor.revents = 0;
        int wait_ms = (int)timeout_to(now, owner->run_deadline);
        if (socket_poll.timeout_ms < (uint32_t)wait_ms) { wait_ms = (int)socket_poll.timeout_ms; }
        if (socket_poll.immediate_work) { wait_ms = 0; }
        if (stop_event != SessionNullHandle && wait_ms > 0) { wait_ms = (int)timeout_to(now, now + (uint32_t)wait_ms); }
        int polled = session_poll(&descriptor, 1, wait_ms);
        if (polled < 0) { record_wsa(owner); return InteropGuestSessionOsError; }
        stop = wait_stop(owner, stop_event);
        if (stop < 0) { return InteropGuestSessionOsError; }
        if (stop > 0) { return InteropGuestSessionStopped; }
        now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        if (now >= owner->run_deadline) { return InteropGuestSessionTimedOut; }
        interop_windows_socket_work_t work;
        now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        result = session_socket_drive(owner->socket_owner, owner->token, socket_poll.poll_cookie,
            now, (uint32_t)descriptor.revents, InteropWindowsSocketMaxRxBudgetBytes,
            InteropWindowsSocketMaxTxBudgetBytes, InteropWindowsSocketMaxSyscallBudget,
            frame, sizeof(frame), &work);
        if (result != InteropWindowsSocketSuccess) { return InteropGuestSessionPeerFailed; }
        if (work.status == InteropWindowsSocketWorkPeerFailure || work.status == InteropWindowsSocketWorkCloseIndeterminate ||
            work.status == InteropWindowsSocketWorkClosePending || work.status == InteropWindowsSocketWorkTransportClosed) {
            return InteropGuestSessionPeerFailed;
        }
        if (work.event.kind == InteropPeerEventReady) {
            if (work.event.capabilities != 0) { return InteropGuestSessionPeerFailed; }
            int valid = validate_header(owner);
            if (valid != InteropGuestSessionSuccess) { return valid; }
        } else if (work.event.kind == InteropPeerEventFeatureFrame) {
            return InteropGuestSessionUnexpectedFeature;
        } else if (work.event.kind == InteropPeerEventDraining || work.event.kind == InteropPeerEventError) {
            return InteropGuestSessionPeerFailed;
        }
        now = current_now(owner);
        if (now == UINT64_MAX) { return InteropGuestSessionOsError; }
        if (now >= owner->run_deadline) { return InteropGuestSessionTimedOut; }
        if (now >= owner->next_heartbeat && work.event.phase == InteropPeerReady) {
            uint64_t observed = 0;
            interop_shm_header_t *header = (interop_shm_header_t *)(uintptr_t)owner->mapping_view.mapping_base;
            if (session_heartbeat_load(header, owner->mapping_view.mapping_base, owner->mapping_view.mapping_length,
                    InteropGuestHeartbeat, &observed) != InteropAtomicStatusOk || observed != owner->last_guest_heartbeat || observed == UINT64_MAX ||
                session_heartbeat_store_next(header, owner->mapping_view.mapping_base, owner->mapping_view.mapping_length,
                    InteropGuestHeartbeat, observed + 1) != InteropAtomicStatusOk) {
                return InteropGuestSessionMappingFailed;
            }
            owner->last_guest_heartbeat = observed + 1;
            owner->next_heartbeat = now + InteropGuestSessionHeartbeatPeriodMs;
        }
    }
    return InteropGuestSessionTimedOut;
}
static void drain_socket(interop_guest_session_owner_t *owner) {
    if (owner->socket_owner == NULL) { return; }
    interop_peer_event_t drain_event = {0};
    uint64_t start = current_now(owner);
    if (start == UINT64_MAX || start > UINT64_MAX - InteropGuestSessionDrainTimeoutMs) { return; }
    (void)session_socket_begin_drain(owner->socket_owner, owner->token, start, &drain_event);
    uint64_t deadline = start + InteropGuestSessionDrainTimeoutMs;
    uint8_t frame[InteropWindowsSocketFrameCapacity];
    while (owner->socket_owner != NULL) {
        uint64_t now = current_now(owner);
        if (now == UINT64_MAX) { break; }
        if (now >= deadline) { break; }
        interop_windows_socket_poll_t socket_poll;
        if (session_socket_poll_events(owner->socket_owner, owner->token, now, &socket_poll) != InteropWindowsSocketSuccess) { break; }
        WSAPOLLFD descriptor = {0};
        descriptor.fd = (SOCKET)socket_poll.socket_value;
        descriptor.events = (short)socket_poll.events;
        uint32_t wait_ms = timeout_to(now, deadline);
        if (socket_poll.timeout_ms < wait_ms) { wait_ms = socket_poll.timeout_ms; }
        if (socket_poll.immediate_work != 0) { wait_ms = 0; }
        int polled = session_poll(&descriptor, 1, (int)wait_ms);
        if (polled < 0) { record_wsa(owner); break; }
        interop_windows_socket_work_t work;
        now = current_now(owner);
        if (now == UINT64_MAX) { break; }
        int driven = session_socket_drive(owner->socket_owner, owner->token, socket_poll.poll_cookie,
            now, (uint32_t)descriptor.revents, InteropWindowsSocketMaxRxBudgetBytes,
            InteropWindowsSocketMaxTxBudgetBytes, InteropWindowsSocketMaxSyscallBudget,
            frame, sizeof(frame), &work);
        if (driven != InteropWindowsSocketSuccess || work.state == InteropWindowsSocketTransportClosed ||
            work.status == InteropWindowsSocketWorkTransportClosed ||
            work.status == InteropWindowsSocketWorkClosePending ||
            work.status == InteropWindowsSocketWorkCloseIndeterminate) { break; }
    }
}

interop_guest_session_result_t interop_guest_session_run(const interop_guest_session_config_t *config,
    interop_guest_session_owner_t **owner_out) {
    if (!local_range_valid(config, sizeof(*config), _Alignof(interop_guest_session_config_t)) ||
        !local_range_valid(owner_out, sizeof(*owner_out), _Alignof(interop_guest_session_owner_t *)) ||
        local_ranges_overlap(config, sizeof(*config), owner_out, sizeof(*owner_out)) || *owner_out != NULL ||
        config->reserved_zero != 0 ||
        config->fresh_exclusive_authorized > 1 || config->connection_token == 0) { return InteropGuestSessionInvalidArgument; }
    if (config->fresh_exclusive_authorized != 1) { return InteropGuestSessionFreshAuthorizationRequired; }
    interop_guest_session_owner_t *owner = (interop_guest_session_owner_t *)session_allocate(1, sizeof(*owner));
    if (owner == NULL) { return InteropGuestSessionOutOfMemory; }
    owner->owner_thread = session_thread_id(); owner->token = config->connection_token;
    owner->operational_result = InteropGuestSessionSuccess; owner->family_handle = SessionNullHandle;
    owner->listener = INVALID_SOCKET; owner->accepted_socket = INVALID_SOCKET;
    WSADATA startup_data;
    memset(&startup_data, 0, sizeof(startup_data));
    int startup_result = session_wsa_startup(MAKEWORD(2, 2), &startup_data);
    if (startup_result != 0) {
        record_wsa_code(owner, startup_result); record_operational(owner, InteropGuestSessionOsError); goto finish;
    }
    owner->winsock_started = 1;
    owner->family_handle = session_open_family();
    if (owner->family_handle == SessionNullHandle || owner->family_handle == SessionInvalidHandle) {
        record_win32(owner); record_operational(owner, InteropGuestSessionOsError); owner->family_handle = SessionNullHandle; goto finish;
    }
    DWORD family = 0, returned = 0;
    if (!session_query_family(owner->family_handle, &family, &returned)) {
        record_win32(owner); record_operational(owner, InteropGuestSessionOsError); goto finish;
    }
    if (returned != sizeof(family) || family == 0 || family > UINT16_MAX) {
        record_win32_code(owner, ERROR_INVALID_DATA); record_operational(owner, InteropGuestSessionOsError); goto finish;
    }
    close_handle_slot(owner);
    if (owner->family_handle != SessionNullHandle) { record_operational(owner, InteropGuestSessionCleanupRequired); goto finish; }
    record_operational(owner, accept_connection(owner, (uint16_t)family, config));
    if (owner->socket_owner != NULL && owner->operational_result == InteropGuestSessionSuccess) {
        record_operational(owner, initialize_mapping(owner, config->device_index));
    }
    if (owner->socket_owner != NULL && owner->operational_result == InteropGuestSessionSuccess) {
        record_operational(owner, run_loop(owner, config->stop_event));
    }
    if (owner->socket_owner != NULL && owner->operational_result != InteropGuestSessionSuccess) { drain_socket(owner); }
finish:
    cleanup(owner, 0);
    if (resources_remain(owner)) { *owner_out = owner; return InteropGuestSessionCleanupRequired; }
    interop_guest_session_result_t result = (interop_guest_session_result_t)owner->operational_result;
    free(owner);
    return result;
}
interop_guest_session_result_t interop_guest_session_retry_cleanup(interop_guest_session_owner_t **slot,
    uint32_t detached) {
    if (!local_range_valid(slot, sizeof(*slot), _Alignof(interop_guest_session_owner_t *)) || detached > 1 || *slot == NULL) {
        return InteropGuestSessionInvalidArgument;
    }
    interop_guest_session_owner_t *owner = *slot;
    if (local_ranges_overlap(slot, sizeof(*slot), owner, sizeof(*owner))) {
        return InteropGuestSessionInvalidArgument;
    }
    if (!is_owner_thread(owner)) { return InteropGuestSessionWrongThread; }
    owner->cleanup_retry_mode = 1;
    cleanup(owner, detached);
    if (resources_remain(owner)) { return InteropGuestSessionCleanupRequired; }
    *slot = NULL; free(owner); return InteropGuestSessionSuccess;
}
interop_guest_session_result_t interop_guest_session_quarantine_socket(
    interop_guest_session_owner_t **slot, interop_guest_session_quarantine_t *out) {
    if (!local_range_valid(slot, sizeof(*slot), _Alignof(interop_guest_session_owner_t *)) ||
        !local_range_valid(out, sizeof(*out), _Alignof(interop_guest_session_quarantine_t)) ||
        local_ranges_overlap(slot, sizeof(*slot), out, sizeof(*out)) || *slot == NULL) {
        return InteropGuestSessionInvalidArgument;
    }
    interop_guest_session_owner_t *owner = *slot;
    if (local_ranges_overlap(slot, sizeof(*slot), owner, sizeof(*owner)) ||
        local_ranges_overlap(out, sizeof(*out), owner, sizeof(*owner))) {
        return InteropGuestSessionInvalidArgument;
    }
    if (!is_owner_thread(owner)) { return InteropGuestSessionWrongThread; }
    interop_guest_session_quarantine_t empty = {0};
    if (memcmp(out, &empty, sizeof(empty)) != 0 || owner->mapping_owner != NULL ||
        owner->family_handle != SessionNullHandle || owner->winsock_started == 0 ||
        (owner->listener != INVALID_SOCKET && owner->listener_close_pending == 0 &&
            owner->listener_close_indeterminate == 0) ||
        (owner->accepted_socket != INVALID_SOCKET && owner->accepted_close_pending == 0 &&
            owner->accepted_close_indeterminate == 0) ||
        (owner->socket_owner != NULL && owner->socket_close_pending == 0 &&
            owner->socket_close_indeterminate == 0)) {
        return InteropGuestSessionInvalidArgument;
    }
    interop_guest_session_quarantine_t quarantine = {0};
    SOCKET *raw_sockets[2] = {&owner->listener, &owner->accepted_socket};
    uint32_t *raw_pending[2] = {&owner->listener_close_pending, &owner->accepted_close_pending};
    uint32_t *raw_indeterminate[2] = {&owner->listener_close_indeterminate, &owner->accepted_close_indeterminate};
    for (size_t index = 0; index < 2; index++) {
        if (*raw_sockets[index] == INVALID_SOCKET) { continue; }
        interop_windows_socket_quarantine_t *record = &quarantine.sockets[quarantine.socket_count++];
        record->socket_value = (uintptr_t)*raw_sockets[index];
        record->wsa_error = *raw_sockets[index] == owner->listener ? owner->listener_close_wsa_error : owner->accepted_close_wsa_error;
        record->close_classification = *raw_pending[index] != 0 ?
            InteropWindowsSocketClosePendingState : InteropWindowsSocketCloseIndeterminateState;
        record->owner_thread_id = owner->owner_thread;
        record->connection_token = owner->token;
    }
    if (owner->socket_owner != NULL) {
        int result = session_socket_quarantine_handoff(&owner->socket_owner, owner->token,
            &quarantine.sockets[quarantine.socket_count]);
        if (result != InteropWindowsSocketSuccess) { return InteropGuestSessionCleanupRequired; }
        quarantine.socket_count++;
    }
    if (quarantine.socket_count == 0) { return InteropGuestSessionInvalidArgument; }
    for (size_t index = 0; index < 2; index++) {
        *raw_sockets[index] = INVALID_SOCKET;
        *raw_pending[index] = 0;
        *raw_indeterminate[index] = 0;
    }
    owner->listener_close_wsa_error = 0;
    owner->accepted_close_wsa_error = 0;
    owner->socket_close_pending = 0;
    owner->socket_close_indeterminate = 0;
    owner->socket_destroy_retryable = 0;
    quarantine.winsock_reference_owned = 1;
    *out = quarantine;
    owner->winsock_started = 0;
    *slot = NULL;
    free(owner);
    return InteropGuestSessionSuccess;
}
interop_guest_session_result_t interop_guest_session_release_quarantine(
    interop_guest_session_quarantine_t *quarantine, uint32_t socket_resolution_assertion) {
    if (!local_range_valid(quarantine, sizeof(*quarantine), _Alignof(interop_guest_session_quarantine_t)) ||
        socket_resolution_assertion != 1 || quarantine->socket_count == 0 || quarantine->socket_count > 3 ||
        quarantine->winsock_reference_owned != 1 || quarantine->reserved_zero != 0) {
        return InteropGuestSessionInvalidArgument;
    }
    for (uint32_t index = 0; index < quarantine->socket_count; index++) {
        const interop_windows_socket_quarantine_t *record = &quarantine->sockets[index];
        if (record->socket_value == 0 || record->reserved_zero != 0 ||
            (record->close_classification != InteropWindowsSocketClosePendingState &&
             record->close_classification != InteropWindowsSocketCloseIndeterminateState) ||
            record->owner_thread_id != quarantine->sockets[0].owner_thread_id) {
            return InteropGuestSessionInvalidArgument;
        }
        for (uint32_t prior = 0; prior < index; prior++) {
            if (quarantine->sockets[prior].socket_value == record->socket_value) {
                return InteropGuestSessionInvalidArgument;
            }
        }
    }
    for (uint32_t index = quarantine->socket_count; index < 3; index++) {
        interop_windows_socket_quarantine_t empty_record = {0};
        if (memcmp(&quarantine->sockets[index], &empty_record, sizeof(empty_record)) != 0) {
            return InteropGuestSessionInvalidArgument;
        }
    }
    if (session_thread_id() != quarantine->sockets[0].owner_thread_id) { return InteropGuestSessionWrongThread; }
    if (session_wsa_cleanup() != 0) {
        quarantine->last_wsa_error = (uint32_t)session_last_wsa_error();
        return InteropGuestSessionOsError;
    }
    memset(quarantine, 0, sizeof(*quarantine));
    return InteropGuestSessionSuccess;
}
interop_guest_session_result_t interop_guest_session_get_state(const interop_guest_session_owner_t *owner,
    interop_guest_session_state_t *state_out) {
    if (owner == NULL || !local_range_valid(state_out, sizeof(*state_out), _Alignof(interop_guest_session_state_t)) ||
        local_ranges_overlap(owner, sizeof(*owner), state_out, sizeof(*state_out))) {
        return InteropGuestSessionInvalidArgument;
    }
    if (!is_owner_thread(owner)) { return InteropGuestSessionWrongThread; }
    publish_state(owner, state_out); return InteropGuestSessionSuccess;
}
