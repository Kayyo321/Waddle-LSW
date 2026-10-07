/** @file channel_windows.c @brief Real overlapped named-pipe lifecycle fixture. */
#include "waddle/venus_channel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static _Alignas(64) uint8_t mapping[4096];
static venus_session_t host;
static venus_session_t guest;
static venus_channel_t host_channel;
static venus_channel_t guest_channel;
static _Atomic uint32_t cancel;
static HANDLE server;
static HANDLE client;
static unsigned pipe_number;

static void fresh(void) {
    wchar_t path[128];
    assert(swprintf(path, 128, L"\\\\.\\pipe\\waddle-venus-%lu-%u", GetCurrentProcessId(),
                    ++pipe_number) > 0);
    server =
        CreateNamedPipeW(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    assert(server != INVALID_HANDLE_VALUE);
    OVERLAPPED connect = {0};
    connect.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(connect.hEvent);
    assert(!ConnectNamedPipe(server, &connect) && GetLastError() == ERROR_IO_PENDING);
    client = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, NULL);
    assert(client != INVALID_HANDLE_VALUE);
    assert(WaitForSingleObject(connect.hEvent, 1000) == WAIT_OBJECT_0);
    DWORD bytes;
    assert(GetOverlappedResult(server, &connect, &bytes, TRUE));
    assert(CloseHandle(connect.hEvent));
    atomic_store_explicit(&cancel, 0, memory_order_release);
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&host, SessionHost, mapping, sizeof(mapping), 91) == RingOk);
    assert(venus_session_init(&guest, SessionGuest, mapping, sizeof(mapping), 0) == RingOk);
    assert(venus_channel_init(&host_channel, &host, (intptr_t)server, &cancel) == RingOk);
    assert(venus_channel_init(&guest_channel, &guest, (intptr_t)client, &cancel) == RingOk);
    assert(venus_channel_deadline(&host_channel, 1000) == RingOk);
    assert(venus_channel_deadline(&guest_channel, 1000) == RingOk);
}
static void cleanup(void) {
    HANDLE host_event = (HANDLE)host_channel.event;
    HANDLE guest_event = (HANDLE)guest_channel.event;
    venus_channel_free(&host_channel);
    venus_channel_free(&guest_channel);
    assert(!host_channel.initialized && !guest_channel.initialized);
    DWORD flags;
    assert(!GetHandleInformation(host_event, &flags));
    assert(!GetHandleInformation(guest_event, &flags));
    assert(GetHandleInformation(server, &flags));
    if (client != INVALID_HANDLE_VALUE) {
        assert(GetHandleInformation(client, &flags));
        assert(CloseHandle(client));
    }
    assert(CloseHandle(server));
    venus_channel_free(&host_channel);
}
static DWORD WINAPI host_handshake(void *context) {
    (void)context;
    assert(venus_channel_handshake(&host_channel) == RingOk);
    return 0;
}
static void handshake(void) {
    HANDLE thread = CreateThread(NULL, 0, host_handshake, NULL, 0, NULL);
    assert(thread);
    assert(venus_channel_handshake(&guest_channel) == RingOk);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
    assert(venus_session_ready(&host) && venus_session_ready(&guest));
}
static void write_peer(const void *input, DWORD length) {
    OVERLAPPED request = {0};
    request.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(request.hEvent);
    DWORD bytes = 0;
    if (!WriteFile(client, input, length, &bytes, &request)) {
        assert(GetLastError() == ERROR_IO_PENDING);
        assert(WaitForSingleObject(request.hEvent, 1000) == WAIT_OBJECT_0);
        assert(GetOverlappedResult(client, &request, &bytes, TRUE));
    }
    assert(bytes == length);
    assert(CloseHandle(request.hEvent));
}
static void test_fragmentation_stop_and_eof(void) {
    fresh();
    handshake();
    assert(venus_channel_wait(&host_channel) == RingOk); /* Pending read cancelled and joined. */
    uint8_t frame[VenusControlBytes];
    const venus_control_t Stop = {.kind = ControlStop,
                                  .reason = StopCancel,
                                  .session_id = 91,
                                  .mapping_bytes = 4096,
                                  .capacity = 64};
    assert(venus_control_encode(&Stop, frame, sizeof(frame)) == RingOk);
    for (size_t index = 0; index < sizeof(frame); index++) {
        write_peer(frame + index, 1);
        assert(venus_channel_wait(&host_channel) ==
               (index + 1 == sizeof(frame) ? RingClosed : RingOk));
    }
    assert(host.reason == StopCancel);
    cleanup();
    fresh();
    handshake();
    assert(venus_channel_stop(&guest_channel, StopDisconnect) == RingOk);
    assert(CloseHandle(client));
    client = INVALID_HANDLE_VALUE;
    assert(venus_channel_wait(&host_channel) == RingClosed);
    assert(host.reason == StopDisconnect);
    cleanup();
    for (int partial = 0; partial < 2; partial++) {
        fresh();
        handshake();
        if (partial) {
            write_peer(frame, 3);
            assert(venus_channel_wait(&host_channel) == RingOk);
        }
        assert(CloseHandle(client));
        client = INVALID_HANDLE_VALUE;
        assert(venus_channel_wait(&host_channel) == (partial ? RingCorrupt : RingClosed));
        assert(host.reason == (partial ? StopProtocol : StopDisconnect));
        cleanup();
    }
}
static void test_deadline_cancel_and_validation(void) {
    fresh();
    assert(venus_channel_deadline(&guest_channel, 3) == RingOk);
    assert(venus_channel_handshake(&guest_channel) == RingTimeout);
    assert(guest.reason == StopDeadline);
    cleanup();
    fresh();
    handshake();
    atomic_store_explicit(&cancel, 1, memory_order_release);
    assert(venus_channel_wait(&host_channel) == RingCancelled);
    assert(host.reason == StopCancel);
    cleanup();
    fresh();
    atomic_store_explicit(&cancel, 1, memory_order_release);
    assert(venus_channel_handshake(&host_channel) == RingCancelled);
    cleanup();
    fresh();
    venus_channel_t empty = {0};
    assert(venus_channel_init(&empty, &host, 0, NULL) == RingInvalid);
    assert(venus_channel_init(&empty, &host, (intptr_t)INVALID_HANDLE_VALUE, NULL) == RingInvalid);
    HANDLE invalid = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(invalid && CloseHandle(invalid));
    assert(venus_channel_init(&empty, &host, (intptr_t)invalid, NULL) == RingInvalid);
    cleanup();
}
static void test_absolute_deadline_and_partial_progress(void) {
    fresh();
    uint64_t now = venus_channel_time_ms();
    uint64_t original = host_channel.deadline_ms;
    assert(now);
    venus_channel_t empty = {0};
    assert(venus_channel_deadline_until(NULL, now) == RingInvalid);
    assert(venus_channel_deadline_until(&empty, now) == RingInvalid);
    assert(venus_channel_deadline_until(&host_channel, 0) == RingInvalid);
    assert(host_channel.deadline_ms == original && host.state == SessionInitialized);
    assert(venus_channel_deadline_until(&host_channel, UINT64_MAX) == RingInvalid);
    assert(host_channel.deadline_ms == original && host.state == SessionInitialized);
    assert(venus_channel_deadline_until(&host_channel, now) == RingTimeout);
    assert(host_channel.deadline_ms == original && host.reason == StopDeadline);
    assert(venus_channel_handshake(&host_channel) == RingInvalid);
    DWORD available = 0;
    assert(PeekNamedPipe(client, NULL, 0, NULL, &available, NULL) && !available);
    atomic_store(&cancel, 1);
    assert(venus_channel_deadline_until(&host_channel, now + 1000) == RingClosed);
    assert(host.reason == StopDeadline && host_channel.deadline_ms == original);
    cleanup();
    fresh();
    original = host_channel.deadline_ms;
    atomic_store(&cancel, 1);
    assert(venus_channel_deadline_until(&host_channel, 1) == RingCancelled);
    assert(host.reason == StopCancel && host_channel.deadline_ms == original);
    cleanup();
    fresh();
    uint64_t deadline = venus_channel_time_ms() + 1000;
    assert(venus_channel_deadline_until(&host_channel, deadline) == RingOk);
    assert(venus_channel_deadline_until(&guest_channel, deadline) == RingOk);
    handshake();
    deadline = venus_channel_time_ms() + 40;
    assert(venus_channel_deadline_until(&host_channel, deadline) == RingOk);
    const uint8_t byte = 0x57;
    write_peer(&byte, 1);
    assert(venus_channel_wait(&host_channel) == RingOk);
    assert(host_channel.received == 1 && host_channel.incoming[0] == byte);
    assert(venus_channel_deadline_until(&host_channel, deadline + 1000) == RingInvalid);
    while (venus_channel_time_ms() < deadline)
        Sleep(1);
    assert(venus_channel_wait(&host_channel) == RingTimeout);
    assert(host_channel.deadline_ms == deadline && host_channel.received == 1);
    assert(host_channel.incoming[0] == byte && host.reason == StopDeadline);
    cleanup();
    for (int writing = 0; writing < 2; writing++) {
        fresh();
        handshake();
        uint8_t bytes[64];
        memset(bytes, 0xa5, sizeof(bytes));
        if (writing)
            assert(venus_ring_write(&guest.region.commands, bytes, sizeof(bytes)) == RingOk);
        venus_channel_t *channel = writing ? &guest_channel : &host_channel;
        deadline = venus_channel_time_ms() + 10;
        assert(venus_channel_deadline_until(channel, deadline) == RingOk);
        venus_ring_status_t status =
            writing ? venus_ring_write_wait(&guest.region.commands, bytes, 1,
                                             venus_channel_wait, channel)
                    : venus_ring_read_wait(&host.region.commands, bytes, 1,
                                            venus_channel_wait, channel);
        assert(status == RingTimeout && channel->deadline_ms == deadline);
        assert(channel->session->reason == StopDeadline);
        assert(atomic_load(&host.region.commands.header->head) == 0);
        assert(atomic_load(&host.region.commands.header->tail) == (writing ? 64u : 0u));
        for (size_t index = 0; index < sizeof(bytes); index++)
            assert(bytes[index] == 0xa5);
        cleanup();
    }
}
int main(void) {
    DWORD initial_handles, final_handles;
    assert(GetProcessHandleCount(GetCurrentProcess(), &initial_handles));
    test_fragmentation_stop_and_eof();
    test_deadline_cancel_and_validation();
    test_absolute_deadline_and_partial_progress();
    assert(GetProcessHandleCount(GetCurrentProcess(), &final_handles));
    assert(final_handles == initial_handles);
    puts("Native Windows overlapped handoff, framing, cancellation and EOF passed");
    return 0;
}
