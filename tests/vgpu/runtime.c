/** @file runtime.c @brief Portable framing and receiver-boundary fault fixtures. */
#include "waddle/venus_dispatch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(64) unsigned char mapping[4096];
static unsigned char scratch[160], peer_input[512], peer_output[512];
static size_t incoming_bytes, incoming_position, outgoing_bytes;
static venus_session_t session;
static venus_channel_t channel;
static venus_rpc_t rpc;
static venus_ring_status_t wait_error, deadline_error, receiver_status;
static int eof, calls;
static uint64_t fence_value;
static int receiver_cookie;

static venus_ring_t *incoming_ring(void) {
    return session.role == SessionHost ? &session.region.commands : &session.region.replies;
}
static venus_ring_t *outgoing_ring(void) {
    return session.role == SessionHost ? &session.region.replies : &session.region.commands;
}
static void drain_peer(void) {
    venus_ring_t *ring = outgoing_ring();
    size_t length = atomic_load(&ring->header->tail) - atomic_load(&ring->header->head);
    if (length) {
        assert(outgoing_bytes + length <= sizeof(peer_output));
        assert(venus_ring_read(ring, peer_output + outgoing_bytes, length) == RingOk);
        outgoing_bytes += length;
    }
}
venus_ring_status_t venus_channel_deadline(venus_channel_t *borrowed, uint32_t timeout) {
    assert(borrowed == &channel && timeout && timeout <= 60000);
    return deadline_error;
}
venus_ring_status_t venus_channel_wait(void *context) {
    assert(context == &channel);
    if (wait_error != RingOk)
        return wait_error;
    drain_peer();
    if (incoming_position < incoming_bytes) {
        venus_ring_t *ring = incoming_ring();
        size_t occupied = atomic_load(&ring->header->tail) - atomic_load(&ring->header->head);
        size_t length = incoming_bytes - incoming_position;
        if (length > ring->capacity - occupied)
            length = ring->capacity - occupied;
        if (length) {
            assert(venus_ring_write(ring, peer_input + incoming_position, length) == RingOk);
            incoming_position += length;
        }
        return RingOk;
    }
    return eof ? RingClosed : RingOk;
}

venus_ring_status_t venus_receiver_capabilities(const venus_receiver_t *receiver, void *output,
                                                size_t length) {
    assert(receiver && length == 160);
    calls++;
    if (receiver_status == RingOk)
        memset(output, 0x42, length);
    return receiver_status;
}
venus_ring_status_t venus_receiver_submit(venus_receiver_t *receiver, const void *input,
                                          size_t length, uint64_t *fence) {
    assert(receiver && input && length >= 8);
    calls++;
    *fence = receiver_status == RingOk ? fence_value : 0;
    return receiver_status;
}
venus_ring_status_t venus_receiver_reply(const venus_receiver_t *receiver, uint64_t offset,
                                         void *output, size_t length) {
    assert(receiver && offset == 0 && length == 4);
    calls++;
    if (receiver_status == RingOk)
        memset(output, 0x42, length);
    return receiver_status;
}
venus_ring_status_t venus_receiver_resource_create(venus_receiver_t *receiver, uint32_t id,
                                                   uint64_t blob, uint64_t bytes, uint32_t flags) {
    assert(receiver && id == 2 && blob == 0 && bytes == 4096 && flags == 1);
    calls++;
    return receiver_status;
}
venus_ring_status_t venus_receiver_resource_free(venus_receiver_t *receiver, uint32_t id) {
    assert(receiver && id == 2);
    calls++;
    return receiver_status;
}
venus_ring_status_t venus_receiver_resource_read(venus_receiver_t *receiver, uint32_t id,
                                                 uint64_t offset, void *output, size_t length) {
    assert(receiver && id == 2 && offset == 0 && length == 4);
    calls++;
    if (receiver_status == RingOk)
        memset(output, 0x42, length);
    return receiver_status;
}
venus_ring_status_t venus_receiver_resource_write(venus_receiver_t *receiver, uint32_t id,
                                                  uint64_t offset, const void *input,
                                                  size_t length) {
    assert(receiver && id == 2 && offset == 0 && length == 4);
    for (size_t index = 0; index < length; index++)
        assert(((const unsigned char *)input)[index] == 0x31);
    calls++;
    return receiver_status;
}
venus_ring_status_t venus_receiver_poll(const venus_receiver_t *receiver) {
    assert(receiver);
    calls++;
    return receiver_status;
}

static void reset(venus_session_role_t role) {
    memset(&session, 0, sizeof(session));
    memset(&channel, 0, sizeof(channel));
    memset(&rpc, 0, sizeof(rpc));
    memset(scratch, 0x5a, sizeof(scratch));
    incoming_bytes = incoming_position = outgoing_bytes = 0;
    wait_error = deadline_error = receiver_status = RingOk;
    eof = calls = 0;
    fence_value = 1;
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&session, role, mapping, sizeof(mapping),
                              role == SessionHost ? 1 : 0) == RingOk);
    /* Handshake transitions/native delivery are covered independently. */
    session.state = SessionReady;
    channel.session = &session;
    channel.initialized = 1;
    assert(venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) == RingOk);
}
static venus_request_t request_for(uint32_t kind) {
    venus_request_t value = {.kind = kind};
    if (kind == RequestSubmit)
        value.payload_bytes = 8;
    if (kind == RequestReply || kind == RequestRead || kind == RequestWrite)
        value.argument_one = 4;
    if (kind >= RequestCreate && kind <= RequestWrite)
        value.resource_id = 2;
    if (kind == RequestCreate) {
        value.flags = 1;
        value.argument_one = 4096;
    }
    if (kind == RequestWrite)
        value.payload_bytes = 4;
    return value;
}
static size_t expected_bytes(uint32_t kind) {
    return kind == RequestCapabilities ? 160 : kind == RequestRead || kind == RequestReply ? 4 : 0;
}
static void prepare(venus_request_t value) {
    assert(venus_request_encode(&value, peer_input, 64) == RingOk);
    memset(peer_input + 64, value.direction ? 0x42 : 0x31, value.payload_bytes);
    incoming_bytes = 64 + value.payload_bytes;
}
static venus_ring_status_t serve(void) {
    return venus_dispatch_serve(&rpc, (venus_receiver_t *)(void *)&receiver_cookie, 1000);
}
static void assert_terminal(venus_ring_status_t result, venus_ring_status_t expected) {
    assert(result == expected);
    assert(!rpc.next_sequence && !venus_session_ready(&session));
    assert(atomic_load(&session.region.commands.header->flags) == 1);
    assert(atomic_load(&session.region.replies.header->flags) == 1);
}
static void host_operations(void) {
    const venus_ring_status_t Results[] = {RingOk, RingAgain, RingInvalid, RingLimit};
    const uint32_t Wire[] = {RequestSuccess, RequestAgain, RequestInvalid, RequestLimit};
    for (uint32_t kind = RequestCapabilities; kind <= RequestPoll; kind++) {
        for (size_t index = 0; index < sizeof(Results) / sizeof(Results[0]); index++) {
            reset(SessionHost);
            venus_request_t request = request_for(kind), response;
            request.sequence = 1;
            prepare(request);
            receiver_status = Results[index];
            assert(serve() == RingOk && rpc.next_sequence == 2 && calls == 1);
            drain_peer();
            assert(venus_request_decode(&response, peer_output, 64) == RingOk);
            assert(response.kind == kind && response.sequence == 1 &&
                   response.status == Wire[index]);
            assert(response.payload_bytes == (index == 0 ? expected_bytes(kind) : 0));
            assert(outgoing_bytes == 64 + response.payload_bytes);
            for (size_t offset = 64; offset < outgoing_bytes; offset++)
                assert(peer_output[offset] == 0x42);
            venus_rpc_free(&rpc);
        }
    }
    for (uint32_t kind = RequestSubmit; kind <= RequestWrite; kind++) {
        if (kind != RequestSubmit && kind != RequestRead && kind != RequestWrite)
            continue;
        reset(SessionHost);
        venus_request_t request = request_for(kind), response;
        request.sequence = 1;
        if (kind == RequestRead)
            request.argument_one = 300;
        else {
            request.payload_bytes = 300;
            if (kind == RequestWrite)
                request.argument_one = 300;
        }
        prepare(request);
        assert(serve() == RingOk && calls == 0 && incoming_position == incoming_bytes);
        drain_peer();
        assert(venus_request_decode(&response, peer_output, 64) == RingOk);
        assert(response.status == RequestLimit);
    }
}
static void host_failures(void) {
    reset(SessionHost);
    assert(venus_dispatch_serve(&rpc, NULL, 1000) == RingInvalid);
    assert(venus_dispatch_serve(NULL, (venus_receiver_t *)(void *)&receiver_cookie, 1000) ==
           RingInvalid);
    for (int mode = 0; mode < 9; mode++) {
        reset(SessionHost);
        venus_request_t request = request_for(mode == 5 ? RequestWrite : RequestPoll);
        request.sequence = mode == 1 ? 2 : 1;
        if (mode == 2)
            request.direction = 1;
        prepare(request);
        if (mode == 0)
            peer_input[0] ^= 1;
        if (mode == 3)
            receiver_status = RingCorrupt;
        if (mode == 4)
            receiver_status = (venus_ring_status_t)99;
        if (mode == 5) {
            incoming_bytes = 66;
            eof = 1;
        }
        if (mode == 6) {
            incoming_bytes = 7;
            eof = 1;
        }
        if (mode == 7) {
            incoming_bytes = 0;
            eof = 1;
        }
        if (mode == 8)
            wait_error = RingCancelled;
        assert_terminal(serve(), mode == 7 ? RingClosed : mode == 8 ? RingCancelled : RingCorrupt);
    }
    reset(SessionHost);
    venus_request_t request = request_for(RequestSubmit);
    request.sequence = 1;
    prepare(request);
    fence_value = 0;
    assert_terminal(serve(), RingCorrupt); /* Bad SDK success cannot escape wire validation. */
}
static void guest_operations(void) {
    unsigned char input[8], output[160];
    memset(input, 0x31, sizeof(input));
    for (uint32_t kind = RequestCapabilities; kind <= RequestPoll; kind++) {
        for (uint32_t status = RequestSuccess; status <= RequestLimit; status++) {
            reset(SessionGuest);
            venus_request_t request = request_for(kind), response;
            venus_request_t reply = {.kind = kind, .direction = 1, .sequence = 1, .status = status};
            if (status == RequestSuccess) {
                reply.payload_bytes = (uint32_t)expected_bytes(kind);
                if (kind == RequestSubmit)
                    reply.argument_zero = 1;
            }
            prepare(reply);
            memset(output, 0x5a, sizeof(output));
            assert(venus_rpc_exchange(&rpc, &request, input, request.payload_bytes, &response,
                                      output, sizeof(output), 1000) == RingOk);
            assert(rpc.next_sequence == 2 && response.status == status);
            drain_peer();
            venus_request_t sent;
            assert(venus_request_decode(&sent, peer_output, 64) == RingOk);
            assert(sent.sequence == 1 && sent.kind == kind &&
                   sent.payload_bytes == request.payload_bytes);
            for (size_t offset = 0; offset < sizeof(output); offset++)
                assert(output[offset] == (offset < reply.payload_bytes ? 0x42 : 0x5a));
        }
    }
}
static void guest_failures(void) {
    unsigned char output[160];
    venus_request_t response;
    for (int mode = 0; mode < 10; mode++) {
        reset(SessionGuest);
        venus_request_t request = request_for(RequestRead);
        venus_request_t reply = {
            .kind = RequestRead, .direction = 1, .sequence = 1, .payload_bytes = 4};
        if (mode == 0)
            reply.sequence = 2;
        if (mode == 1)
            reply.kind = RequestReply;
        if (mode == 2) {
            reply.direction = 0;
            reply.resource_id = 2;
            reply.argument_one = 4;
            reply.payload_bytes = 0;
        }
        if (mode == 3)
            reply.payload_bytes = 8;
        prepare(reply);
        if (mode == 4)
            peer_input[0] ^= 1;
        if (mode == 5) {
            incoming_bytes = 66;
            eof = 1;
        }
        if (mode == 6) {
            incoming_bytes = 7;
            eof = 1;
        }
        if (mode == 7)
            wait_error = RingTimeout;
        if (mode == 8)
            rpc.next_sequence = UINT64_MAX;
        if (mode == 9)
            channel.received = 1;
        memset(output, 0x5a, sizeof(output));
        assert_terminal(
            venus_rpc_exchange(&rpc, &request, NULL, 0, &response, output, sizeof(output), 1000),
            mode == 7 ? RingTimeout : RingCorrupt);
        const venus_request_t Empty = {0};
        assert(memcmp(&response, &Empty, sizeof(response)) == 0);
        for (size_t offset = 0; offset < sizeof(output); offset++)
            assert(output[offset] == 0x5a);
    }
}
static void local_errors(void) {
    reset(SessionGuest);
    venus_request_t request = request_for(RequestRead), response;
    unsigned char output[160];
    assert(venus_rpc_init(NULL, &channel, scratch, sizeof(scratch)) == RingInvalid);
    venus_rpc_t empty = {0};
    assert(venus_rpc_init(&empty, NULL, scratch, sizeof(scratch)) == RingInvalid);
    assert(venus_rpc_init(&empty, &channel, NULL, sizeof(scratch)) == RingInvalid);
    assert(venus_rpc_init(&empty, &channel, scratch, 159) == RingInvalid);
    assert(venus_rpc_init(&empty, &channel, scratch, VenusRequestMaxPayload + 1) == RingInvalid);
    assert(venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) == RingInvalid);
    channel.initialized = 0;
    assert(venus_rpc_init(&empty, &channel, scratch, sizeof(scratch)) == RingInvalid);
    channel.initialized = 1;
    session.state = SessionInitialized;
    assert(venus_rpc_init(&empty, &channel, scratch, sizeof(scratch)) == RingInvalid);
    session.state = SessionReady;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, NULL, output, 160, 1000) == RingInvalid);
    assert(venus_rpc_exchange(NULL, &request, NULL, 0, &response, output, 160, 1000) ==
           RingInvalid);
    assert(venus_rpc_exchange(&empty, &request, NULL, 0, &response, output, 160, 1000) ==
           RingInvalid);
    assert(venus_rpc_exchange(&rpc, NULL, NULL, 0, &response, output, 160, 1000) == RingInvalid);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 160, 1000) == RingInvalid);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, output, 3, 1000) == RingInvalid);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 1, &response, output, 160, 1000) ==
           RingInvalid);
    request.argument_one = 161;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, output, 160, 1000) ==
           RingInvalid);
    request = request_for(RequestWrite);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 4, &response, output, 160, 1000) ==
           RingInvalid);
    request.argument_one = request.payload_bytes = 161;
    assert(venus_rpc_exchange(&rpc, &request, output, 161, &response, output, 160, 1000) ==
           RingInvalid);
    request = request_for(RequestPoll);
    request.sequence = 1;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 1000) == RingInvalid);
    request.sequence = 0;
    request.direction = 1;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 1000) == RingInvalid);
    request.direction = 0;
    request.kind = 0;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 1000) == RingInvalid);
    request.kind = RequestPoll;
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 0) == RingInvalid);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 60001) == RingInvalid);
    assert(serve() == RingInvalid); /* Role mismatch. */
    assert(rpc.next_sequence == 1 && incoming_position == 0 && outgoing_bytes == 0);
    deadline_error = RingClosed;
    assert_terminal(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 1000),
                    RingClosed);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 1000) == RingClosed);
    reset(SessionHost);
    venus_session_close(&session, StopDisconnect);
    assert_terminal(serve(), RingClosed);
    venus_rpc_free(NULL);
    venus_rpc_free(&rpc);
    venus_rpc_free(&rpc);
}
int main(void) {
    host_operations();
    host_failures();
    guest_operations();
    guest_failures();
    local_errors();
    puts("Sequential bounded runtime operations and failure paths passed");
    return 0;
}
