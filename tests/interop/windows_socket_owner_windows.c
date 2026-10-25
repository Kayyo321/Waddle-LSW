/** @file windows_socket_owner_windows.c
 * @brief Native WinSock loopback handshake test for the connected byte owner.
 * @ownership Fixture owns sockets until each successful owner create; owner
 * cleanup and Winsock shutdown are completed on every successful test path.
 */
#include "../../src/interop/windows_socket_owner.h"
#include "waddle/interop_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static const uint8_t Version[] = "1.2.3";

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "windows_socket_owner_windows: %s (WSA=%d)\n",
                message, WSAGetLastError());
        abort();
    }
}

static void make_loopback_pair(SOCKET *client_out, SOCKET *accepted_out)
{
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET accepted = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(listener != INVALID_SOCKET, "create listener");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    expect(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0,
           "bind listener");
    expect(listen(listener, 1) == 0, "listen");
    expect(getsockname(listener, (struct sockaddr *)&address, &address_length) == 0,
           "read listener address");
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(client != INVALID_SOCKET, "create client");
    expect(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0,
           "connect client");
    accepted = accept(listener, NULL, NULL);
    expect(accepted != INVALID_SOCKET, "accept client");
    expect(closesocket(listener) == 0, "close listener");
    *client_out = client;
    *accepted_out = accepted;
}

static void set_nonblocking(SOCKET socket_value)
{
    u_long nonblocking = 1;
    expect(SetHandleInformation((HANDLE)(uintptr_t)socket_value,
                                HANDLE_FLAG_INHERIT, 0) != 0,
           "clear socket inheritance");
    expect(ioctlsocket(socket_value, FIONBIO, &nonblocking) == 0,
           "prepare nonblocking socket");
}

typedef struct wrong_thread_call_t {
    interop_windows_socket_t *owner;
    uint64_t token;
    int result;
    interop_windows_socket_poll_t output;
} wrong_thread_call_t;

static DWORD WINAPI wrong_thread_probe(LPVOID opaque)
{
    wrong_thread_call_t *call = (wrong_thread_call_t *)opaque;
    call->result = interop_windows_socket_poll_events(call->owner, call->token,
                                                       GetTickCount64(), &call->output);
    return 0;
}

static void verify_wrong_thread_rejected(interop_windows_socket_t *owner, uint64_t token)
{
    wrong_thread_call_t call;
    interop_windows_socket_poll_t before;
    HANDLE thread;

    memset(&call, 0, sizeof(call));
    call.owner = owner;
    call.token = token;
    memset(&call.output, 0xA5, sizeof(call.output));
    memcpy(&before, &call.output, sizeof(before));
    thread = CreateThread(NULL, 0, wrong_thread_probe, &call, 0, NULL);
    expect(thread != NULL, "create wrong-thread probe");
    expect(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
           "join wrong-thread probe");
    expect(CloseHandle(thread) != 0, "close wrong-thread probe handle");
    expect(call.result == InteropWindowsSocketWrongThread,
           "reject owner call from another thread");
    expect(memcmp(&call.output, &before, sizeof(before)) == 0,
           "wrong-thread output remains unchanged");
}

static interop_windows_socket_config_t make_config(uint32_t role, uint64_t token)
{
    interop_windows_socket_config_t config;
    memset(&config, 0, sizeof(config));
    config.peer_config.role = role;
    config.peer_config.connection_token = token;
    config.peer_config.accepted_at_ms = GetTickCount64();
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1;
    config.peer_config.local_capabilities = InteropCapWindows | InteropCapInput;
    config.drain_timeout_ms = 1000;
    return config;
}

static void drive_one(interop_windows_socket_t *owner, uint64_t token,
                      size_t rx_budget, size_t tx_budget,
                      interop_windows_socket_work_t *work)
{
    interop_windows_socket_poll_t poll_state;
    WSAPOLLFD descriptor;
    uint8_t frame[InteropWindowsSocketFrameCapacity];
    int result;
    uint64_t now_ms = GetTickCount64();
    memset(&poll_state, 0, sizeof(poll_state));
    expect(interop_windows_socket_poll_events(owner, token, now_ms, &poll_state) ==
           InteropWindowsSocketSuccess, "owner poll snapshot");
    descriptor.fd = (SOCKET)poll_state.socket_value;
    descriptor.events = (SHORT)poll_state.events;
    expect((poll_state.events & ~(uint32_t)(POLLRDNORM | POLLWRNORM)) == 0,
           "WSAPoll input interests exclude output-only error flags");
    descriptor.revents = 0;
    result = WSAPoll(&descriptor, 1,
                     poll_state.immediate_work != 0 ? 0 : (INT)poll_state.timeout_ms);
    expect(result >= 0, "WSAPoll");
    now_ms = GetTickCount64();
    expect(interop_windows_socket_drive(owner, token, poll_state.poll_cookie,
           now_ms, (uint32_t)(unsigned short)descriptor.revents,
           rx_budget, tx_budget, InteropWindowsSocketMaxSyscallBudget,
           frame, sizeof(frame), work) == InteropWindowsSocketSuccess,
           "owner drive");
}

static void drive_until_event(interop_windows_socket_t *owner, uint64_t token,
                              uint32_t event_kind,
                              interop_windows_socket_work_t *work)
{
    uint64_t deadline_ms = GetTickCount64() + 2000u;
    do {
        drive_one(owner, token, InteropWindowsSocketMaxRxBudgetBytes,
                  InteropWindowsSocketMaxTxBudgetBytes, work);
        if (work->event.kind == event_kind)
            return;
    } while (GetTickCount64() < deadline_ms);
    fprintf(stderr, "windows_socket_owner_windows: timed out waiting for event %u\n",
            event_kind);
    abort();
}

int main(void)
{
    WSADATA winsock_data;
    SOCKET guest_socket = INVALID_SOCKET;
    SOCKET host_socket = INVALID_SOCKET;
    interop_windows_socket_t *guest = NULL;
    interop_windows_socket_t *host = NULL;
    interop_windows_socket_config_t guest_config = make_config(InteropPeerGuest, 0x7001);
    interop_windows_socket_config_t host_config = make_config(InteropPeerHost, 0x7002);
    interop_peer_event_t event;
    interop_windows_socket_work_t work;
    const uint64_t guest_token = 0x7001;
    const uint64_t host_token = 0x7002;
    int status = 1;

    expect(WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0, "WSAStartup");
    make_loopback_pair(&guest_socket, &host_socket);
    set_nonblocking(guest_socket);
    set_nonblocking(host_socket);
    expect(interop_windows_socket_create(guest_socket, &guest_config, &guest) ==
           InteropWindowsSocketSuccess, "create guest owner");
    guest_socket = INVALID_SOCKET;
    expect(interop_windows_socket_create(host_socket, &host_config, &host) ==
           InteropWindowsSocketSuccess, "create host owner");
    host_socket = INVALID_SOCKET;
    verify_wrong_thread_rejected(guest, guest_token);

    expect(interop_windows_socket_mapping_result(guest, guest_token,
           GetTickCount64(), 0x7010,
           InteropLayoutVersion, 1, &event) == InteropWindowsSocketSuccess,
           "complete guest mapping before Hello");
    drive_until_event(guest, guest_token, InteropPeerEventNone, &work);
    drive_until_event(host, host_token, InteropPeerEventValidateMapping, &work);
    expect(interop_windows_socket_mapping_result(host, host_token,
           GetTickCount64(), 0x7010,
           InteropLayoutVersion, 1, &event) == InteropWindowsSocketSuccess,
           "complete host mapping validation");
    drive_until_event(host, host_token, InteropPeerEventNone, &work);
    drive_until_event(guest, guest_token, InteropPeerEventReady, &work);

    expect(interop_windows_socket_abort(guest, guest_token, &work) ==
           InteropWindowsSocketSuccess, "abort guest transport");
    expect(interop_windows_socket_abort(host, host_token, &work) ==
           InteropWindowsSocketSuccess, "abort host transport");
    expect(interop_windows_socket_destroy(&guest) == InteropWindowsSocketSuccess,
           "destroy guest owner");
    expect(interop_windows_socket_destroy(&host) == InteropWindowsSocketSuccess,
           "destroy host owner");
    status = 0;

    if (guest_socket != INVALID_SOCKET)
        (void)closesocket(guest_socket);
    if (host_socket != INVALID_SOCKET)
        (void)closesocket(host_socket);
    if (guest != NULL)
        (void)interop_windows_socket_destroy(&guest);
    if (host != NULL)
        (void)interop_windows_socket_destroy(&host);
    expect(WSACleanup() == 0, "WSACleanup");
    if (status == 0)
        puts("windows_socket_owner_windows: native AF_INET owner test passed");
    return status;
}
