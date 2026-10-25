/** @file session_control_windows_integration.c
 * @brief Native loopback composition of the production guest session path.
 *
 * Only Viosock discovery/address calls are translated to AF_INET loopback.
 * The controller, connected Windows socket owner, pure peer ABI, layout
 * validator, mapping initializer, and native atomics remain production code.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "session_control_windows.h"
#include "windows_socket_owner.h"
#include "interop_layout.h"
#include "interop_native_atomic.h"
#include "waddle/interop_peer.h"
#include "waddle/interop_protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Borrow the native fixture map base while its production map owner lives.
 * @return Zero when closed; otherwise the read-only actual 2 GiB VirtualAlloc base.
 * @note Host fixture thread may read it while the controller is active; both
 *       threads must be joined before proof-gated mapping cleanup.
 */
extern uintptr_t interop_guest_native_fixture_mapping_base(void);

typedef struct synthetic_vsock_address_t {
    uint16_t family;
    uint16_t reserved;
    uint32_t port;
    uint32_t cid;
} synthetic_vsock_address_t;

static volatile LONG listener_port;
static volatile LONG pending_listener_port;
static const uint8_t AgentVersion[] = "0.0.0";

static HANDLE native_guest_open_viosock(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    (void)path; (void)access; (void)sharing; (void)security;
    (void)creation; (void)flags; (void)template_file;
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}
static BOOL native_guest_query_viosock_family(HANDLE handle, DWORD code, LPVOID input,
    DWORD input_bytes, LPVOID output, DWORD output_bytes, DWORD *returned,
    LPOVERLAPPED overlapped) {
    (void)input; (void)input_bytes; (void)overlapped;
    if (handle == NULL || code != InteropGuestSessionViosockFamilyIoctl ||
        output == NULL || output_bytes < sizeof(DWORD) || returned == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *(DWORD *)output = AF_INET;
    *returned = sizeof(DWORD);
    return TRUE;
}
static int WSAAPI native_guest_bind_loopback(SOCKET socket_value,
    const struct sockaddr *address, int address_length) {
    const synthetic_vsock_address_t *source = (const synthetic_vsock_address_t *)address;
    struct sockaddr_in destination;
    int result;
    if (source == NULL || address_length != sizeof(*source) || source->family != AF_INET ||
        source->port != 5002u || source->cid != UINT32_MAX) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = 0;
    result = bind(socket_value, (const struct sockaddr *)&destination, sizeof(destination));
    if (result == 0) {
        int length = sizeof(destination);
        if (getsockname(socket_value, (struct sockaddr *)&destination, &length) != 0) {
            return SOCKET_ERROR;
        }
        InterlockedExchange(&pending_listener_port, (LONG)ntohs(destination.sin_port));
    }
    return result;
}
static int WSAAPI native_guest_listen(SOCKET socket_value, int backlog) {
    int result = listen(socket_value, backlog);
    if (result == 0) {
        InterlockedExchange(&listener_port,
            InterlockedCompareExchange(&pending_listener_port, 0, 0));
    }
    return result;
}
static int WSAAPI native_guest_peer_address(SOCKET socket_value,
    struct sockaddr *address, int *address_length) {
    struct sockaddr_in remote;
    int length = sizeof(remote);
    if (address == NULL || address_length == NULL ||
        *address_length < (int)sizeof(synthetic_vsock_address_t)) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if (getpeername(socket_value, (struct sockaddr *)&remote, &length) != 0) { return SOCKET_ERROR; }
    if (remote.sin_family != AF_INET || remote.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        WSASetLastError(WSAEAFNOSUPPORT);
        return SOCKET_ERROR;
    }
    synthetic_vsock_address_t synthetic = {AF_INET, 0, 5002u, InteropGuestSessionHostCid};
    memcpy(address, &synthetic, sizeof(synthetic));
    *address_length = sizeof(synthetic);
    return 0;
}

#define CreateFileW native_guest_open_viosock
#define DeviceIoControl native_guest_query_viosock_family
#define bind native_guest_bind_loopback
#define listen native_guest_listen
#define getpeername native_guest_peer_address
#include "../../src/interop/session_control_windows.c"
#undef CreateFileW
#undef DeviceIoControl
#undef bind
#undef listen
#undef getpeername

typedef enum fixture_mode_t {
    FixtureModeReadyStop = 1,
    FixtureModeEofBeforeReady = 2,
    FixtureModeTimeoutBeforeReady = 3
} fixture_mode_t;

typedef struct fixture_host_t {
    HANDLE stop_event;
    fixture_mode_t mode;
    volatile LONG hello_seen;
    volatile LONG mapping_validated;
    volatile LONG ack_bytes_sent;
    volatile LONG ready_seen;
    volatile LONG guest_heartbeat_seen;
    volatile LONG transport_eof_seen;
    volatile LONG failed;
    uint32_t wsa_error;
} fixture_host_t;

static void fixture_fail(fixture_host_t *host, uint32_t error) {
    if (InterlockedCompareExchange(&host->failed, 0, 0) == 0) {
        host->wsa_error = error;
        InterlockedExchange(&host->failed, 1);
    }
}

static int fixture_validate_header(uintptr_t base, uint64_t epoch) {
    uint8_t snapshot_bytes[4096];
    uint64_t guest = 0, host = 0;
    if (base == 0) { return 0; }
    memcpy(snapshot_bytes, (const void *)base, 56);
    memcpy(snapshot_bytes + 72, (const void *)(base + 72), sizeof(snapshot_bytes) - 72);
    interop_shm_header_t *header = (interop_shm_header_t *)base;
    if (interop_heartbeat_load(header, base, UINT64_C(0x80000000), InteropGuestHeartbeat, &guest) != InteropAtomicStatusOk ||
        interop_heartbeat_load(header, base, UINT64_C(0x80000000), InteropHostHeartbeat, &host) != InteropAtomicStatusOk) {
        return 0;
    }
    memcpy(snapshot_bytes + 56, &guest, sizeof(guest));
    memcpy(snapshot_bytes + 64, &host, sizeof(host));
    interop_shm_header_snapshot_t snapshot;
    return interop_layout_validate_shm_header(snapshot_bytes, sizeof(snapshot_bytes), base,
        UINT64_C(0x80000000), epoch, &snapshot) == InteropLayoutStatusOk;
}

static int fixture_handle_host_event(fixture_host_t *host, interop_peer_t *peer,
    uint64_t token, interop_peer_event_t *event) {
    if (event->kind == InteropPeerEventValidateMapping) {
        InterlockedExchange(&host->hello_seen, 1);
        if (host->mode == FixtureModeEofBeforeReady || host->mode == FixtureModeTimeoutBeforeReady) {
            return 1;
        }
        if (event->session_epoch == 0 || event->layout_version != InteropLayoutVersion ||
            !fixture_validate_header(interop_guest_native_fixture_mapping_base(), event->session_epoch)) {
            fixture_fail(host, ERROR_INVALID_DATA);
            return -1;
        }
        InterlockedExchange(&host->mapping_validated, 1);
        interop_peer_event_t result_event = {0};
        if (interop_peer_mapping_result(peer, token, GetTickCount64(), event->session_epoch,
                event->layout_version, 1, &result_event) != InteropPeerSuccess) {
            fixture_fail(host, ERROR_INVALID_DATA);
            return -1;
        }
        return 0;
    }
    if (event->kind == InteropPeerEventReady) {
        InterlockedExchange(&host->ready_seen, 1);
    }
    if (event->kind == InteropPeerEventError) {
        fixture_fail(host, event->error_code);
        return -1;
    }
    return 0;
}

static int fixture_pump_received(fixture_host_t *host, interop_peer_t *peer,
    uint64_t token, const uint8_t *bytes, size_t length) {
    size_t offset = 0;
    uint8_t event_bytes[InteropWindowsSocketFrameCapacity];
    while (offset < length) {
        size_t consumed = 0;
        interop_peer_event_t event = {0};
        int result = interop_peer_feed(peer, token, bytes + offset, length - offset,
            &consumed, &event);
        if (result != InteropPeerSuccess || consumed == 0) { return 0; }
        offset += consumed;
        for (size_t step = 0; step < InteropWindowsSocketMaxInternalStepsPerDrive; step++) {
            memset(&event, 0, sizeof(event));
            result = interop_peer_next_event(peer, token, GetTickCount64(), event_bytes,
                sizeof(event_bytes), &event);
            if (result != InteropPeerSuccess) { return 0; }
            if (event.kind == InteropPeerEventNone) { break; }
            int handled = fixture_handle_host_event(host, peer, token, &event);
            if (handled != 0) { return handled > 0 ? 2 : 0; }
        }
    }
    return 1;
}

static int fixture_flush_host_peer(fixture_host_t *host, interop_peer_t *peer, uint64_t token,
    SOCKET socket_value) {
    uint8_t tx[InteropWindowsSocketFrameCapacity];
    for (size_t step = 0; step < InteropWindowsSocketMaxInternalStepsPerDrive; step++) {
        size_t pending = 0;
        if (interop_peer_tx_bytes(peer, token, &pending) != InteropPeerSuccess || pending == 0) { return 1; }
        size_t copied = 0;
        if (interop_peer_copy_tx(peer, token, tx, sizeof(tx), &copied) != InteropPeerSuccess || copied == 0) { return 0; }
        int written = send(socket_value, (const char *)tx, (int)copied, 0);
        if (written == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) { return 1; }
            fixture_fail(host, (uint32_t)error);
            return 0;
        }
        interop_peer_event_t event = {0};
        if (interop_peer_ack_tx(peer, token, GetTickCount64(), (size_t)written, &event) != InteropPeerSuccess) {
            fixture_fail(host, ERROR_INVALID_DATA);
            return 0;
        }
        if (InterlockedCompareExchange(&host->mapping_validated, 0, 0) != 0) {
            if (InterlockedCompareExchange(&host->hello_seen, 0, 0) == 0) {
                fixture_fail(host, ERROR_INVALID_DATA);
                return 0;
            }
            InterlockedExchange(&host->ack_bytes_sent, 1);
        }
        if (event.kind == InteropPeerEventReady) { InterlockedExchange(&host->ready_seen, 1); }
        if ((size_t)written < copied) { return 1; }
        if (InterlockedCompareExchange(&host->failed, 0, 0) != 0) { return 0; }
    }
    return 1;
}

static DWORD WINAPI fixture_host_thread(LPVOID opaque) {
    fixture_host_t *host = (fixture_host_t *)opaque;
    WSADATA startup_data;
    SOCKET socket_value = INVALID_SOCKET;
    interop_peer_t *peer = NULL;
    uint64_t token = UINT64_C(0x6f776e65722d7631);
    uint64_t deadline = GetTickCount64() + 20000u;
    uint8_t rx[8192];
    int peer_created = 0;
    int startup_error = WSAStartup(MAKEWORD(2, 2), &startup_data);
    int winsock_started = startup_error == 0;
    if (!winsock_started) { fixture_fail(host, (uint32_t)startup_error); goto finish; }
    while (InterlockedCompareExchange(&listener_port, 0, 0) == 0 && GetTickCount64() < deadline) { Sleep(1); }
    if (InterlockedCompareExchange(&listener_port, 0, 0) == 0) { fixture_fail(host, WSAETIMEDOUT); goto finish; }
    socket_value = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket_value == INVALID_SOCKET) { fixture_fail(host, WSAGetLastError()); goto finish; }
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons((u_short)InterlockedCompareExchange(&listener_port, 0, 0));
    if (connect(socket_value, (const struct sockaddr *)&destination, sizeof(destination)) != 0) {
        fixture_fail(host, WSAGetLastError()); goto finish;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket_value, FIONBIO, &nonblocking) != 0) { fixture_fail(host, WSAGetLastError()); goto finish; }
    interop_peer_config_t config = {0};
    config.role = InteropPeerHost;
    config.connection_token = token;
    config.accepted_at_ms = GetTickCount64();
    config.agent_version = AgentVersion;
    config.agent_version_bytes = sizeof(AgentVersion) - 1;
    config.local_capabilities = 0;
    if (interop_peer_create(&config, &peer) != InteropPeerSuccess) { fixture_fail(host, ERROR_NOT_ENOUGH_MEMORY); goto finish; }
    peer_created = 1;
    while (GetTickCount64() < deadline && InterlockedCompareExchange(&host->failed, 0, 0) == 0) {
        WSAPOLLFD descriptor;
        descriptor.fd = socket_value;
        descriptor.events = POLLRDNORM;
        descriptor.revents = 0;
        int ready = WSAPoll(&descriptor, 1, 25);
        if (ready == SOCKET_ERROR) { fixture_fail(host, WSAGetLastError()); break; }
        if ((descriptor.revents & POLLRDNORM) != 0) {
            int received = recv(socket_value, (char *)rx, sizeof(rx), 0);
            if (received == 0) {
                InterlockedExchange(&host->transport_eof_seen, 1);
                break;
            }
            if (received == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK) { fixture_fail(host, (uint32_t)error); break; }
            } else {
                int pumped = fixture_pump_received(host, peer, token, rx, (size_t)received);
                if (pumped == 0) { fixture_fail(host, ERROR_INVALID_DATA); break; }
                if (pumped == 2 && host->mode == FixtureModeEofBeforeReady) { break; }
                if (pumped == 2 && host->mode == FixtureModeTimeoutBeforeReady) {
                    /* Leave the peer unanswered until its bounded Hello deadline. */
                }
            }
        }
        interop_peer_event_t tick_event = {0};
        if (interop_peer_tick(peer, token, GetTickCount64(), &tick_event) != InteropPeerSuccess) {
            fixture_fail(host, ERROR_INVALID_DATA); break;
        }
        if (tick_event.kind != InteropPeerEventNone) {
            int handled = fixture_handle_host_event(host, peer, token, &tick_event);
            if (handled < 0) { break; }
        }
        if (host->mode == FixtureModeReadyStop &&
            InterlockedCompareExchange(&host->ready_seen, 0, 0) != 0) {
            uint64_t guest_heartbeat = 0;
            uintptr_t base = interop_guest_native_fixture_mapping_base();
            if (base != 0 && interop_heartbeat_load((const interop_shm_header_t *)base,
                    base, UINT64_C(0x80000000), InteropGuestHeartbeat,
                    &guest_heartbeat) == InteropAtomicStatusOk && guest_heartbeat >= 2) {
                InterlockedExchange(&host->guest_heartbeat_seen, 1);
                SetEvent(host->stop_event);
            }
        }
        if (!fixture_flush_host_peer(host, peer, token, socket_value)) { break; }
    }
    if (GetTickCount64() >= deadline && InterlockedCompareExchange(&host->transport_eof_seen, 0, 0) == 0) {
        fixture_fail(host, WSAETIMEDOUT);
    }
finish:
    if (socket_value != INVALID_SOCKET) {
        shutdown(socket_value, SD_BOTH);
        closesocket(socket_value);
    }
    if (peer_created) {
        interop_peer_event_t lost = {0};
        if (interop_peer_transport_lost(peer, token, &lost) != InteropPeerSuccess) { fixture_fail(host, ERROR_INVALID_DATA); }
        if (interop_peer_destroy(&peer) != InteropPeerSuccess) { fixture_fail(host, ERROR_INVALID_DATA); }
    }
    if (peer != NULL) { fixture_fail(host, ERROR_INVALID_DATA); }
    if (winsock_started && WSACleanup() != 0) { fixture_fail(host, (uint32_t)WSAGetLastError()); }
    return 0;
}

static void run_native_composition_case(fixture_mode_t mode) {
    fixture_host_t host = {0};
    host.mode = mode;
    host.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(host.stop_event != NULL);
    InterlockedExchange(&listener_port, 0);
    InterlockedExchange(&pending_listener_port, 0);
    HANDLE host_thread = CreateThread(NULL, 0, fixture_host_thread, &host, 0, NULL);
    assert(host_thread != NULL);

    interop_guest_session_config_t config = {0};
    config.device_index = 0;
    config.fresh_exclusive_authorized = 1; /* Fixture owns a fresh VirtualAlloc mapping. */
    config.connection_token = UINT64_C(0x6f776e65722d7631);
    config.stop_event = host.stop_event;
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_result_t result = interop_guest_session_run(&config, &owner);
    DWORD wait_result = WaitForSingleObject(host_thread, 25000);
    assert(wait_result == WAIT_OBJECT_0);
    assert(CloseHandle(host_thread));
    interop_guest_session_state_t state = {0};
    interop_guest_session_result_t state_result = InteropGuestSessionInvalidArgument;
    if (owner != NULL) { state_result = interop_guest_session_get_state(owner, &state); }
    if (InterlockedCompareExchange(&host.failed, 0, 0) != 0 || result != InteropGuestSessionCleanupRequired ||
        InterlockedCompareExchange(&host.hello_seen, 0, 0) == 0) {
        fprintf(stderr,
            "native session fixture mode=%u result=%u wait=%lu host_failed=%ld wsa_error=%lu "
            "hello=%ld map_valid=%ld ack=%ld ready=%ld heartbeat=%ld eof=%ld owner=%p state_result=%u "
            "operational=%u cleanup_errors=%u win32_error=%u map_owned=%u map_published=%u socket_owner=%u\n",
            (unsigned)mode, (unsigned)result, (unsigned long)wait_result,
            (long)InterlockedCompareExchange(&host.failed, 0, 0), (unsigned long)host.wsa_error,
            (long)InterlockedCompareExchange(&host.hello_seen, 0, 0),
            (long)InterlockedCompareExchange(&host.mapping_validated, 0, 0),
            (long)InterlockedCompareExchange(&host.ack_bytes_sent, 0, 0),
            (long)InterlockedCompareExchange(&host.ready_seen, 0, 0),
            (long)InterlockedCompareExchange(&host.guest_heartbeat_seen, 0, 0),
            (long)InterlockedCompareExchange(&host.transport_eof_seen, 0, 0),
            (void *)owner, (unsigned)state_result, (unsigned)state.operational_result,
            (unsigned)state.cleanup_error_count, (unsigned)state.last_win32_error,
            (unsigned)state.mapping_owner_owned, (unsigned)state.mapping_view_published,
            (unsigned)state.socket_owner_owned);
    }
    assert(InterlockedCompareExchange(&host.failed, 0, 0) == 0);
    assert(InterlockedCompareExchange(&host.hello_seen, 0, 0) == 1);
    assert(InteropGuestSessionCleanupRequired == result);
    assert(owner != NULL);

    assert(state_result == InteropGuestSessionSuccess);
    assert(state.mapping_owner_owned == 1 && state.mapping_view_published == 1);
    assert(state.winsock_started == 0 && state.family_handle_owned == 0 &&
        state.listener_owned == 0 && state.accepted_socket_owned == 0 &&
        state.socket_owner_owned == 0);
    if (mode == FixtureModeReadyStop) {
        assert(InterlockedCompareExchange(&host.mapping_validated, 0, 0) == 1);
        assert(InterlockedCompareExchange(&host.ack_bytes_sent, 0, 0) == 1);
        assert(InterlockedCompareExchange(&host.ready_seen, 0, 0) == 1);
        assert(InterlockedCompareExchange(&host.guest_heartbeat_seen, 0, 0) == 1);
    } else {
        assert(InterlockedCompareExchange(&host.ready_seen, 0, 0) == 0);
        assert(InterlockedCompareExchange(&host.ack_bytes_sent, 0, 0) == 0);
        assert(InterlockedCompareExchange(&host.mapping_validated, 0, 0) == 0);
        assert(state.operational_result != InteropGuestSessionSuccess);
    }
    /* Joining the sole host peer owner is the fixture's honest local quiescence point. */
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);
    assert(interop_guest_native_fixture_mapping_base() == 0);
    assert(CloseHandle(host.stop_event));
}

/** @brief Run the three bounded local socket/mapping composition scenarios. */
int main(void) {
    run_native_composition_case(FixtureModeReadyStop);
    run_native_composition_case(FixtureModeEofBeforeReady);
    run_native_composition_case(FixtureModeTimeoutBeforeReady);
    puts("session_control_windows native composition tests passed");
    return 0;
}
