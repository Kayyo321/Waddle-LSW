/** @file runtime.c @brief Portable framing and receiver-boundary fault
 * fixtures. */
#include "waddle/venus_capabilities.h"
#include "waddle/venus_dispatch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(64) unsigned char mapping[4096];
static unsigned char scratch[160], peer_input[2048], peer_output[512];
static size_t incoming_bytes, incoming_position, outgoing_bytes;
static venus_session_t session;
static venus_channel_t channel;
static venus_rpc_t rpc;
static venus_ring_status_t wait_error, deadline_error, receiver_status;
static int eof, calls;
static int health_calls, health_after;
static venus_ring_status_t health_status;
static uint64_t fence_value;
static unsigned char host_capabilities[160];
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
        memcpy(output, host_capabilities, length);
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

venus_ring_status_t venus_receiver_gpu_fence(venus_receiver_t *receiver, uint32_t timeline,
                                             uint64_t *fence) {
    assert(receiver && timeline == 1);
    calls++;
    *fence = receiver_status == RingOk ? fence_value : 0;
    return receiver_status;
}
venus_ring_status_t venus_receiver_gpu_poll(const venus_receiver_t *receiver, uint32_t timeline,
                                            uint64_t fence) {
    assert(receiver && timeline == 1 && fence == 1);
    calls++;
    return receiver_status;
}

venus_ring_status_t venus_receiver_health(venus_receiver_t *receiver,
                                          const _Atomic uint32_t *cancel) {
    assert(receiver && cancel == channel.cancel);
    health_calls++;
    return health_after && health_calls < health_after ? RingOk : health_status;
}

static void reset_buffer(venus_session_role_t role, void *buffer, uint32_t bytes) {
    memset(&session, 0, sizeof(session));
    memset(&channel, 0, sizeof(channel));
    memset(&rpc, 0, sizeof(rpc));
    memset(scratch, 0x5a, sizeof(scratch));
    incoming_bytes = incoming_position = outgoing_bytes = 0;
    wait_error = deadline_error = receiver_status = RingOk;
    eof = calls = health_calls = health_after = 0;
    health_status = RingOk;
    fence_value = 1;
    memset(host_capabilities, 0, sizeof(host_capabilities));
    const uint32_t Profile[] = {1, VenusPinnedXmlVersion, 1, 3, 1};
    memcpy(host_capabilities, Profile, sizeof(Profile));
    host_capabilities[20] = 1;
    host_capabilities[68] = 3;
    host_capabilities[152] = 1;
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    assert(venus_session_init(&session, role, mapping, sizeof(mapping),
                              role == SessionHost ? 1 : 0) == RingOk);
    /* Handshake transitions/native delivery are covered independently. */
    session.state = SessionReady;
    channel.session = &session;
    channel.initialized = 1;
    assert(venus_rpc_init(&rpc, &channel, buffer, bytes) == RingOk);
    rpc.negotiated = 1;
}
static void reset(venus_session_role_t role) { reset_buffer(role, scratch, sizeof(scratch)); }
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
    if (kind == RequestGpuFence || kind == RequestGpuPoll) {
        value.argument_zero = 1;
        if (kind == RequestGpuPoll)
            value.argument_one = 1;
    }
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
    venus_ring_status_t result =
        venus_dispatch_serve(&rpc, (venus_receiver_t *)(void *)&receiver_cookie, 1000);
    assert(!rpc.monitor && !rpc.monitor_context);
    return result;
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
    for (uint32_t kind = RequestCapabilities; kind <= RequestGpuPoll; kind++) {
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
                assert(peer_output[offset] ==
                       (kind == RequestCapabilities ? host_capabilities[offset - 64] : 0x42));
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
static void unbound_presentation(void) {
    unsigned char presentation[1216];
    for (unsigned negotiated = 0; negotiated < 2; negotiated++) {
        for (uint32_t kind = RequestPresent; kind <= RequestPresentPoll; kind++) {
            reset_buffer(SessionHost, presentation, sizeof(presentation));
            rpc.negotiated = negotiated;
            venus_request_t request = {.kind = kind, .sequence = 1, .argument_zero = 1}, response;
            if (kind == RequestPresent) {
                request.payload_bytes = sizeof(presentation);
                request.argument_one = 1;
            }
            prepare(request);
            assert(serve() == RingOk && !calls && incoming_position == incoming_bytes);
            drain_peer();
            assert(venus_request_decode(&response, peer_output, 64) == RingOk);
            assert(response.status == RequestInvalid && !response.payload_bytes);
            venus_rpc_free(&rpc);
        }
    }
}
static int presentation_calls, presentation_busy, pump_calls;
static venus_ring_status_t presentation_status, pump_status;
static venus_ring_status_t bound_submit(void *context, const void *bytes, size_t length,
                                        uint32_t timeline, uint64_t fence) {
    assert(context == &receiver_cookie && bytes && length == 1216 && timeline == 1 && fence == 1);
    const unsigned char *input = bytes;
    for (size_t index = 0; index < length; index++)
        assert(input[index] == 0x31);
    presentation_calls++;
    return presentation_status;
}
static venus_ring_status_t bound_take(void *context, uint64_t frame, void *bytes, size_t length) {
    assert(context == &receiver_cookie && frame == 1 && bytes && length == 32);
    presentation_calls++;
    if (presentation_status == RingOk)
        memset(bytes, 0x42, length);
    return presentation_status;
}
static int bound_busy(const void *context, uint32_t resource) {
    assert(context == &receiver_cookie && resource == 2);
    presentation_calls++;
    return presentation_busy;
}
static venus_ring_status_t bound_pump(void *context) {
    assert(context == &receiver_cookie);
    pump_calls++;
    return pump_status;
}
static void bound_presentation(void) {
    unsigned char presentation[1216];
    const venus_dispatch_presentation_t Binding = {.context = &receiver_cookie,
                                                   .submit = bound_submit,
                                                   .take = bound_take,
                                                   .resource_busy = bound_busy,
                                                   .pump = bound_pump};
    for (unsigned field = 0; field < 5; field++) {
        reset(SessionHost);
        venus_dispatch_presentation_t invalid = Binding;
        switch (field) {
        case 0:
            invalid.context = NULL;
            break;
        case 1:
            invalid.submit = NULL;
            break;
        case 2:
            invalid.take = NULL;
            break;
        case 3:
            invalid.resource_busy = NULL;
            break;
        case 4:
            invalid.pump = NULL;
            break;
        }
        assert(venus_dispatch_serve_presented(&rpc, (venus_receiver_t *)&receiver_cookie, &invalid,
                                              1000) == RingInvalid);
        assert(!incoming_position && !outgoing_bytes && !calls);
    }
    for (uint32_t kind = RequestPresent; kind <= RequestPresentPoll; kind++) {
        reset_buffer(SessionHost, presentation, sizeof(presentation));
        rpc.negotiated = 0;
        presentation_calls = 0;
        pump_status = RingOk;
        venus_request_t request = {.kind = kind, .sequence = 1, .argument_zero = 1}, response;
        if (kind == RequestPresent) {
            request.payload_bytes = sizeof(presentation);
            request.argument_one = 1;
        }
        prepare(request);
        assert(venus_dispatch_serve_presented(&rpc, (venus_receiver_t *)&receiver_cookie, &Binding,
                                              1000) == RingOk);
        assert(!calls && !presentation_calls && !rpc.negotiated);
        drain_peer();
        assert(venus_request_decode(&response, peer_output, 64) == RingOk);
        assert(response.status == RequestInvalid && !response.payload_bytes);
    }
    const venus_ring_status_t Results[] = {RingOk,    RingAgain,  RingInvalid,
                                           RingLimit, RingClosed, RingCorrupt};
    const uint32_t Wire[] = {RequestSuccess, RequestAgain, RequestInvalid, RequestLimit};
    for (uint32_t kind = RequestPresent; kind <= RequestPresentPoll; kind++) {
        for (unsigned index = 0; index < sizeof(Results) / sizeof(*Results); index++) {
            reset_buffer(SessionHost, presentation, sizeof(presentation));
            presentation_calls = pump_calls = 0;
            presentation_status = Results[index];
            pump_status = RingOk;
            venus_request_t request = {.kind = kind, .sequence = 1, .argument_zero = 1}, response;
            if (kind == RequestPresent) {
                request.payload_bytes = sizeof(presentation);
                request.argument_one = 1;
            }
            prepare(request);
            venus_ring_status_t status = venus_dispatch_serve_presented(
                &rpc, (venus_receiver_t *)&receiver_cookie, &Binding, 1000);
            assert(!rpc.monitor && !rpc.monitor_context && !calls && presentation_calls == 1);
            assert(pump_calls && incoming_position == incoming_bytes);
            if (index >= 4) {
                assert_terminal(status, RingCorrupt);
                continue;
            }
            assert(status == RingOk);
            drain_peer();
            assert(venus_request_decode(&response, peer_output, 64) == RingOk);
            assert(response.status == Wire[index]);
            assert(response.payload_bytes == (!index && kind == RequestPresentPoll ? 32 : 0));
            for (size_t offset = 64; offset < outgoing_bytes; offset++)
                assert(peer_output[offset] == 0x42);
        }
    }
    for (presentation_busy = 0; presentation_busy < 2; presentation_busy++) {
        reset(SessionHost);
        presentation_calls = pump_calls = 0;
        pump_status = RingOk;
        venus_request_t request = request_for(RequestFree), response;
        request.sequence = 1;
        prepare(request);
        assert(venus_dispatch_serve_presented(&rpc, (venus_receiver_t *)&receiver_cookie, &Binding,
                                              1000) == RingOk);
        assert(presentation_calls == 1 && calls == !presentation_busy);
        drain_peer();
        assert(venus_request_decode(&response, peer_output, 64) == RingOk);
        assert(response.status == (presentation_busy ? RequestAgain : RequestSuccess));
    }
    for (unsigned mode = 0; mode < 3; mode++) {
        reset(SessionHost);
        presentation_calls = pump_calls = 0;
        pump_status = mode ? RingCorrupt : RingClosed;
        if (mode == 2)
            health_status = RingTimeout;
        venus_request_t request = request_for(RequestPoll);
        request.sequence = 1;
        prepare(request);
        venus_ring_status_t status = venus_dispatch_serve_presented(
            &rpc, (venus_receiver_t *)&receiver_cookie, &Binding, 1000);
        assert_terminal(status, mode == 2 ? RingTimeout : pump_status);
        assert(!rpc.monitor && !rpc.monitor_context && !presentation_calls && !calls);
        assert(mode == 2 ? !pump_calls : pump_calls);
    }
    reset(SessionGuest);
    venus_request_t request = {.kind = RequestPresentPoll, .argument_zero = 1}, response;
    venus_request_t reply = {
        .kind = RequestPresentPoll, .direction = 1, .sequence = 1, .payload_bytes = 32};
    unsigned char output[32];
    prepare(reply);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, output, 31, 1000) == RingInvalid);
    assert(!incoming_position && !outgoing_bytes);
    assert(venus_rpc_exchange(&rpc, &request, NULL, 0, &response, output, 32, 1000) == RingOk);
    assert(response.payload_bytes == 32);
    for (unsigned index = 0; index < sizeof(output); index++)
        assert(output[index] == 0x42);
    venus_rpc_free(&rpc);
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
static void health_failures(void) {
    for (int mode = 0; mode < 3; mode++) {
        reset(SessionHost);
        venus_request_t request = request_for(RequestPoll);
        request.sequence = 1;
        prepare(request);
        health_status = mode == 1 ? RingCancelled : RingTimeout;
        if (mode == 2) {
            incoming_bytes = 7;
            health_after = 2; /* Expire during nested ring backpressure retry. */
        }
        assert_terminal(serve(), mode == 1 ? RingCancelled : RingTimeout);
        assert(calls == 0 && session.reason == (mode == 1 ? StopCancel : StopDeadline));
    }
}

static void guest_operations(void) {
    unsigned char input[8], output[160];
    memset(input, 0x31, sizeof(input));
    for (uint32_t kind = RequestCapabilities; kind <= RequestGpuPoll; kind++) {
        for (uint32_t status = RequestSuccess; status <= RequestLimit; status++) {
            reset(SessionGuest);
            venus_request_t request = request_for(kind), response;
            venus_request_t reply = {.kind = kind, .direction = 1, .sequence = 1, .status = status};
            if (status == RequestSuccess) {
                reply.payload_bytes = (uint32_t)expected_bytes(kind);
                if (kind == RequestSubmit || kind == RequestGpuFence)
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
static void negotiation(void) {
    for (uint32_t kind = RequestSubmit; kind <= RequestGpuPoll; kind++) {
        reset(SessionHost);
        rpc.negotiated = 0;
        venus_request_t request = request_for(kind), response;
        request.sequence = 1;
        prepare(request);
        assert(serve() == RingOk && !calls && !rpc.negotiated);
        drain_peer();
        assert(venus_request_decode(&response, peer_output, 64) == RingOk);
        assert(response.status == RequestInvalid);
    }
    for (int mode = 0; mode < 8; mode++) {
        reset(SessionHost);
        rpc.negotiated = mode == 1;
        venus_request_t request = {.kind = RequestNegotiate, .sequence = 1, .payload_bytes = 160};
        prepare(request);
        memcpy(peer_input + 64, host_capabilities, 160);
        if (mode == 2)
            peer_input[64 + 16] = 2; /* Invalid peer flag. */
        if (mode == 3)
            peer_input[64] = 2; /* Peer version mismatch. */
        if (mode == 4)
            host_capabilities[16] = 2;
        if (mode == 5)
            host_capabilities[0] = 2;
        if (mode == 6)
            receiver_status = RingAgain;
        if (mode == 7)
            receiver_status = RingCorrupt;
        venus_ring_status_t result = serve();
        if (mode == 7) {
            assert_terminal(result, RingCorrupt);
            continue;
        }
        assert(result == RingOk && rpc.negotiated == (mode <= 1));
        drain_peer();
        venus_request_t response;
        assert(venus_request_decode(&response, peer_output, 64) == RingOk);
        assert(response.status == (mode == 0   ? RequestSuccess
                                   : mode == 6 ? RequestAgain
                                               : RequestInvalid));
    }
    reset(SessionGuest);
    rpc.negotiated = 0;
    venus_request_t request = {.kind = RequestNegotiate, .payload_bytes = 160}, response;
    venus_request_t reply = {.kind = RequestNegotiate, .direction = 1, .sequence = 1};
    prepare(reply);
    assert(venus_rpc_exchange(&rpc, &request, host_capabilities, 160, &response, NULL, 0, 1000) ==
           RingOk);
    assert(rpc.negotiated);
    venus_rpc_free(&rpc);
    assert(!rpc.negotiated);
}

int main(void) {
    negotiation();
    host_operations();
    unbound_presentation();
    bound_presentation();
    host_failures();
    health_failures();
    guest_operations();
    guest_failures();
    local_errors();
    puts("Sequential bounded runtime operations and failure paths passed");
    return 0;
}
