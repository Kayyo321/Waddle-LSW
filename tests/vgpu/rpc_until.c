/** @file rpc_until.c @brief Real fragmented RPC bytes under one absolute deadline. */
#include "waddle/venus_rpc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
typedef HANDLE thread_t;
#define ThreadReturn DWORD WINAPI
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef pthread_t thread_t;
#define ThreadReturn void *
#endif

static _Alignas(64) unsigned char mapping[4096];
static unsigned char scratch[160];
static venus_session_t host, guest;
static venus_channel_t host_channel, guest_channel;
static venus_rpc_t rpc;
static intptr_t streams[2];
static _Atomic uint32_t cancel;
static unsigned peer_mode;

static void join_thread(thread_t thread) {
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
#else
    assert(pthread_join(thread, NULL) == 0);
#endif
}
static ThreadReturn host_handshake(void *context) {
    (void)context;
    assert(venus_channel_handshake(&host_channel) == RingOk);
    return 0;
}
static void fresh(void) {
#ifdef _WIN32
    static unsigned number;
    wchar_t name[128];
    assert(swprintf(name, 128, L"\\\\.\\pipe\\waddle-rpc-until-%lu-%u",
                    GetCurrentProcessId(), ++number) > 0);
    HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    assert(server != INVALID_HANDLE_VALUE);
    OVERLAPPED connect = {0};
    connect.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(connect.hEvent);
    assert(!ConnectNamedPipe(server, &connect) && GetLastError() == ERROR_IO_PENDING);
    HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    assert(client != INVALID_HANDLE_VALUE);
    assert(WaitForSingleObject(connect.hEvent, 1000) == WAIT_OBJECT_0);
    DWORD bytes;
    assert(GetOverlappedResult(server, &connect, &bytes, TRUE));
    assert(CloseHandle(connect.hEvent));
    streams[0] = (intptr_t)server;
    streams[1] = (intptr_t)client;
#else
    int descriptors[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, descriptors) == 0);
    streams[0] = descriptors[0];
    streams[1] = descriptors[1];
#endif
    atomic_store(&cancel, 0);
    memset(&rpc, 0, sizeof(rpc));
    memset(scratch, 0xa5, sizeof(scratch));
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&host, SessionHost, mapping, sizeof(mapping), 71) == RingOk);
    assert(venus_session_init(&guest, SessionGuest, mapping, sizeof(mapping), 0) == RingOk);
    assert(venus_channel_init(&host_channel, &host, streams[0], &cancel) == RingOk);
    assert(venus_channel_init(&guest_channel, &guest, streams[1], &cancel) == RingOk);
    uint64_t deadline = venus_channel_time_ms() + 1000;
    assert(venus_channel_deadline_until(&host_channel, deadline) == RingOk);
    assert(venus_channel_deadline_until(&guest_channel, deadline) == RingOk);
    thread_t thread;
#ifdef _WIN32
    thread = CreateThread(NULL, 0, host_handshake, NULL, 0, NULL);
    assert(thread);
#else
    assert(pthread_create(&thread, NULL, host_handshake, NULL) == 0);
#endif
    assert(venus_channel_handshake(&guest_channel) == RingOk);
    join_thread(thread);
    assert(venus_rpc_init(&rpc, &guest_channel, scratch, sizeof(scratch)) == RingOk);
}
static void cleanup(void) {
    venus_rpc_free(&rpc);
    venus_channel_free(&guest_channel);
    venus_channel_free(&host_channel);
    assert(!rpc.channel && !guest_channel.initialized && !host_channel.initialized);
#ifdef _WIN32
    assert(CloseHandle((HANDLE)streams[0]));
    assert(CloseHandle((HANDLE)streams[1]));
#else
    assert(close((int)streams[0]) == 0);
    assert(close((int)streams[1]) == 0);
#endif
}
static ThreadReturn peer_reply(void *context) {
    (void)context;
    unsigned char header[VenusRequestHeaderBytes];
    assert(venus_ring_read_wait(&host.region.commands, header, sizeof(header),
                               venus_channel_wait, &host_channel) == RingOk);
    venus_request_t request;
    assert(venus_request_decode(&request, header, sizeof(header)) == RingOk);
    assert(request.kind == RequestReply && request.argument_one == 96 && request.sequence == 1);
    if (peer_mode == 3) {
        atomic_store_explicit(&cancel, 1, memory_order_release);
        return 0;
    }
    venus_request_t response = {.kind = RequestReply, .sequence = 1,
                                .direction = 1, .payload_bytes = 96};
    assert(venus_request_encode(&response, header, sizeof(header)) == RingOk);
    assert(venus_ring_write_wait(&host.region.replies, header,
        peer_mode == 1 ? 8 : sizeof(header), venus_channel_wait, &host_channel) == RingOk);
    if (peer_mode == 1)
        return 0;
    unsigned char payload[64];
    memset(payload, 0x42, sizeof(payload));
    assert(venus_ring_write_wait(&host.region.replies, payload,
        peer_mode == 2 ? 32 : sizeof(payload), venus_channel_wait, &host_channel) == RingOk);
    if (!peer_mode)
        assert(venus_ring_write_wait(&host.region.replies, payload, 32,
                                    venus_channel_wait, &host_channel) == RingOk);
    return 0;
}
static void real_fragmented_exchanges(void) {
    for (peer_mode = 0; peer_mode < 4; peer_mode++) {
        fresh();
        thread_t thread;
#ifdef _WIN32
        thread = CreateThread(NULL, 0, peer_reply, NULL, 0, NULL);
        assert(thread);
#else
        assert(pthread_create(&thread, NULL, peer_reply, NULL) == 0);
#endif
        unsigned char output[98];
        memset(output, 0xa5, sizeof(output));
        venus_request_t request = {.kind = RequestReply, .argument_one = 96}, response;
        uint64_t deadline = venus_channel_time_ms() + (peer_mode ? 50 : 500);
        venus_ring_status_t status = venus_rpc_exchange_until(&rpc, &request, NULL, 0,
            &response, output + 1, 96, deadline, 500);
        join_thread(thread);
        assert(status == (!peer_mode ? RingOk : peer_mode == 3 ? RingCancelled : RingTimeout));
        assert(guest_channel.deadline_ms == deadline);
        assert(output[0] == 0xa5 && output[97] == 0xa5);
        const venus_request_t Empty = {0};
        if (peer_mode) {
            assert(!memcmp(&response, &Empty, sizeof(response)) && !rpc.next_sequence);
            assert(guest.state == SessionClosed);
            assert(atomic_load(&guest.region.commands.header->flags) == VenusRingClosed);
            assert(atomic_load(&guest.region.replies.header->flags) == VenusRingClosed);
        } else
            assert(response.payload_bytes == 96 && rpc.next_sequence == 2);
        for (size_t index = 1; index <= 96; index++)
            assert(output[index] == (peer_mode ? 0xa5 : 0x42));
        cleanup();
    }
}
static void no_peer_and_blocked_publication(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        fresh();
        unsigned char output[96], occupied[64];
        memset(output, 0xa5, sizeof(output));
        memset(occupied, 0x57, sizeof(occupied));
        if (mode == 1)
            assert(venus_ring_write(&guest.region.commands, occupied, sizeof(occupied)) == RingOk);
        venus_request_t request = {.kind = RequestReply, .argument_one = 96}, response;
        uint64_t old_deadline = guest_channel.deadline_ms;
        uint64_t now = venus_channel_time_ms();
        uint64_t deadline = mode == 2 ? now : now + (mode == 0 ? 500 : 20);
        assert(venus_rpc_exchange_until(&rpc, &request, NULL, 0, &response,
                                       output, sizeof(output), deadline,
                                       mode == 0 ? 20 : 500) == RingTimeout);
        assert(!rpc.next_sequence && guest.reason == StopDeadline);
        const venus_request_t Empty = {0};
        assert(!memcmp(&response, &Empty, sizeof(response)));
        if (mode == 0)
            assert(guest_channel.deadline_ms < deadline &&
                   venus_channel_time_ms() >= guest_channel.deadline_ms);
        else
            assert(guest_channel.deadline_ms == (mode == 2 ? old_deadline : deadline));
        assert(atomic_load(&guest.region.commands.header->head) == 0);
        assert(atomic_load(&guest.region.commands.header->tail) == (mode == 2 ? 0u : 64u));
        for (size_t index = 0; index < sizeof(output); index++)
            assert(output[index] == 0xa5);
        cleanup();
    }
}
int main(void) {
#ifdef _WIN32
    DWORD initial_handles, final_handles;
    assert(GetProcessHandleCount(GetCurrentProcess(), &initial_handles));
#endif
    real_fragmented_exchanges();
    no_peer_and_blocked_publication();
#ifdef _WIN32
    assert(GetProcessHandleCount(GetCurrentProcess(), &final_handles));
    assert(initial_handles == final_handles);
#endif
    puts("Real absolute RPC fragmentation, cap, expiration and cancellation passed");
    return 0;
}
