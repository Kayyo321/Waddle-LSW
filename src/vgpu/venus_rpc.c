/** @file venus_rpc.c @brief Private bounded sequential ring framing. */
#include "venus_rpc_internal.h"
#include <string.h>

venus_ring_status_t venus_rpc_init(venus_rpc_t *rpc, venus_channel_t *channel, void *buffer,
                                   uint32_t bytes) {
    if (!rpc || rpc->channel || !channel || !channel->initialized ||
        !venus_session_ready(channel->session) || !buffer || bytes < 160 ||
        bytes > VenusRequestMaxPayload)
        return RingInvalid;
    *rpc = (venus_rpc_t){
        .channel = channel, .buffer = buffer, .buffer_bytes = bytes, .next_sequence = 1};
    return RingOk;
}

void venus_rpc_free(venus_rpc_t *rpc) {
    if (!rpc)
        return;
    if (rpc->channel)
        venus_session_close(rpc->channel->session, StopDisconnect);
    memset(rpc, 0, sizeof(*rpc));
}

venus_ring_status_t venus_rpc_fail(venus_rpc_t *rpc, venus_ring_status_t status) {
    venus_stop_reason_t reason = StopProtocol;
    if (status == RingClosed)
        reason = StopDisconnect;
    else if (status == RingCancelled)
        reason = StopCancel;
    else if (status == RingTimeout)
        reason = StopDeadline;
    venus_session_close(rpc->channel->session, reason);
    rpc->next_sequence = 0;
    return status;
}

venus_ring_status_t venus_rpc_begin(venus_rpc_t *rpc, venus_session_role_t role,
                                    uint32_t timeout_ms) {
    if (!rpc || !rpc->channel || rpc->channel->session->role != role || !timeout_ms ||
        timeout_ms > 60000)
        return RingInvalid;
    if (!rpc->next_sequence || !venus_session_ready(rpc->channel->session))
        return venus_rpc_fail(rpc, RingClosed);
    if (rpc->next_sequence == UINT64_MAX)
        return venus_rpc_fail(rpc, RingCorrupt);
    /* Never reset a deadline while a partial control frame is outstanding. */
    if (rpc->channel->received)
        return venus_rpc_fail(rpc, RingCorrupt);
    venus_ring_status_t result = venus_channel_deadline(rpc->channel, timeout_ms);
    return result == RingOk ? RingOk : venus_rpc_fail(rpc, result);
}

static venus_ring_status_t runtime_wait(void *context) {
    venus_rpc_t *rpc = context;
    if (rpc->monitor) {
        venus_ring_status_t result = rpc->monitor(rpc->monitor_context);
        if (result != RingOk)
            return result;
    }
    return venus_channel_wait(rpc->channel);
}

venus_ring_status_t venus_rpc_transfer(venus_rpc_t *rpc, void *bytes, size_t length, int writing,
                                       int payload) {
    venus_session_t *session = rpc->channel->session;
    int commands = (session->role == SessionGuest) == writing;
    venus_ring_t *ring = commands ? &session->region.commands : &session->region.replies;
    size_t completed = 0;
    while (completed < length) {
        size_t chunk = length - completed;
        if (chunk > ring->capacity)
            chunk = ring->capacity;
        venus_ring_status_t result = runtime_wait(rpc);
        if (result == RingOk) {
            unsigned char *position = (unsigned char *)bytes + completed;
            result = writing ? venus_ring_write_wait(ring, position, chunk, runtime_wait, rpc)
                             : venus_ring_read_wait(ring, position, chunk, runtime_wait, rpc);
        }
        if (result != RingOk) {
            if (!writing && result == RingClosed &&
                (payload || completed ||
                 atomic_load_explicit(&ring->header->tail, memory_order_acquire) !=
                     atomic_load_explicit(&ring->header->head, memory_order_acquire)))
                result = RingCorrupt;
            return venus_rpc_fail(rpc, result);
        }
        completed += chunk;
    }
    return RingOk;
}

static size_t response_bytes(const venus_request_t *request) {
    if (request->kind == RequestCapabilities)
        return 160;
    if (request->kind == RequestPresentPoll)
        return 32;
    if (request->kind == RequestReply || request->kind == RequestRead)
        return (size_t)request->argument_one;
    return 0;
}

static venus_ring_status_t begin_until(venus_rpc_t *rpc, uint64_t deadline_ms,
                                       uint32_t timeout_ms, uint64_t *first_now) {
    if (rpc->channel->session->role != SessionGuest || !deadline_ms || !timeout_ms ||
        timeout_ms > 60000)
        return RingInvalid;
    if (!rpc->next_sequence || !venus_session_ready(rpc->channel->session))
        return venus_rpc_fail(rpc, RingClosed);
    if (rpc->next_sequence == UINT64_MAX || rpc->channel->received)
        return venus_rpc_fail(rpc, RingCorrupt);
    if (rpc->channel->cancel && atomic_load_explicit(rpc->channel->cancel, memory_order_acquire))
        return venus_rpc_fail(rpc, RingCancelled);
    uint64_t now = venus_channel_time_ms();
    if (!now || now > UINT64_MAX - timeout_ms)
        return venus_rpc_fail(rpc, RingClosed);
    if (now >= deadline_ms)
        return venus_rpc_fail(rpc, RingTimeout);
    uint64_t cap_deadline = now + timeout_ms;
    if (cap_deadline < deadline_ms)
        deadline_ms = cap_deadline;
    *first_now = now;
    venus_ring_status_t result = venus_channel_deadline_until(rpc->channel, deadline_ms);
    return result == RingOk || result == RingInvalid ? result : venus_rpc_fail(rpc, result);
}

static venus_ring_status_t check_completion(venus_rpc_t *rpc, uint64_t first_now) {
    if (!venus_session_ready(rpc->channel->session))
        return venus_rpc_fail(rpc, RingClosed);
    if (rpc->channel->cancel && atomic_load_explicit(rpc->channel->cancel, memory_order_acquire))
        return venus_rpc_fail(rpc, RingCancelled);
    uint64_t now = venus_channel_time_ms();
    if (!now || now < first_now)
        return venus_rpc_fail(rpc, RingClosed);
    if (now >= rpc->channel->deadline_ms)
        return venus_rpc_fail(rpc, RingTimeout);
    return RingOk;
}

static venus_ring_status_t exchange_once(venus_rpc_t *rpc, const venus_request_t *request,
                                       const void *input, size_t length, venus_request_t *response,
                                       void *output, size_t capacity, uint64_t deadline_ms,
                                       uint32_t timeout_ms, int absolute) {
    if (!response)
        return RingInvalid;
    memset(response, 0, sizeof(*response));
    if (!rpc || !rpc->channel || !request || request->sequence || request->direction ||
        length != request->payload_bytes || (length && !input) || length > rpc->buffer_bytes)
        return RingInvalid;
    venus_request_t offered = *request;
    offered.sequence = rpc->next_sequence;
    unsigned char header[VenusRequestHeaderBytes];
    if (venus_request_encode(&offered, header, sizeof(header)) != RingOk)
        return rpc->next_sequence ? RingInvalid : RingClosed;
    size_t expected = response_bytes(request);
    if (expected > rpc->buffer_bytes || expected > capacity || (expected && !output))
        return RingInvalid;
    uint64_t first_now = 0;
    venus_ring_status_t result = absolute ? begin_until(rpc, deadline_ms, timeout_ms, &first_now)
                                         : venus_rpc_begin(rpc, SessionGuest, timeout_ms);
    if (result != RingOk)
        return result;
    if (length)
        memcpy(rpc->buffer, input, length);
    result = venus_rpc_transfer(rpc, header, sizeof(header), 1, 0);
    if (result == RingOk)
        result = venus_rpc_transfer(rpc, rpc->buffer, length, 1, 1);
    if (result == RingOk)
        result = venus_rpc_transfer(rpc, header, sizeof(header), 0, 0);
    if (result != RingOk)
        return result;
    venus_request_t received;
    if (venus_request_decode(&received, header, sizeof(header)) != RingOk ||
        received.direction != 1 || received.kind != offered.kind ||
        received.sequence != offered.sequence ||
        (received.status == RequestSuccess && received.payload_bytes != expected))
        return venus_rpc_fail(rpc, RingCorrupt);
    result = venus_rpc_transfer(rpc, rpc->buffer, received.payload_bytes, 0, 1);
    if (result != RingOk)
        return result;
    if (absolute) {
        result = check_completion(rpc, first_now);
        if (result != RingOk)
            return result;
    }
    if (received.payload_bytes)
        memcpy(output, rpc->buffer, received.payload_bytes);
    if (received.kind == RequestNegotiate && received.status == RequestSuccess)
        rpc->negotiated = 1;
    *response = received;
    rpc->next_sequence++;
    return RingOk;
}

venus_ring_status_t venus_rpc_exchange(venus_rpc_t *rpc, const venus_request_t *request,
                                       const void *input, size_t length, venus_request_t *response,
                                       void *output, size_t capacity, uint32_t timeout_ms) {
    return exchange_once(rpc, request, input, length, response, output, capacity, 0, timeout_ms, 0);
}

venus_ring_status_t venus_rpc_exchange_until(venus_rpc_t *rpc, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity,
    uint64_t deadline_ms, uint32_t timeout_ms) {
    return exchange_once(rpc, request, input, length, response, output, capacity,
                         deadline_ms, timeout_ms, 1);
}
