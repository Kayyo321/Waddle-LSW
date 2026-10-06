/** @file venus_dispatch.c @brief Validated bounded receiver request routing. */
#include "waddle/venus_dispatch.h"
#include "venus_rpc_internal.h"
#include "waddle/venus_capabilities.h"

static venus_ring_status_t negotiate(venus_receiver_t *receiver, venus_rpc_t *rpc) {
    if (rpc->negotiated)
        return RingInvalid;
    venus_capabilities_t guest, host;
    unsigned char host_bytes[VenusCapabilityBytes];
    if (venus_capabilities_decode(&guest, rpc->buffer, VenusCapabilityBytes) != RingOk ||
        venus_capabilities_compatible(&guest) != RingOk)
        return RingInvalid;
    venus_ring_status_t result =
        venus_receiver_capabilities(receiver, host_bytes, sizeof(host_bytes));
    if (result != RingOk)
        return result;
    if (venus_capabilities_decode(&host, host_bytes, sizeof(host_bytes)) != RingOk ||
        venus_capabilities_compatible(&host) != RingOk)
        return RingInvalid;
    rpc->negotiated = 1;
    return RingOk;
}

static venus_ring_status_t dispatch(venus_receiver_t *receiver, venus_rpc_t *rpc,
                                    const venus_request_t *request, venus_request_t *response) {
    if (request->kind == RequestNegotiate)
        return negotiate(receiver, rpc);
    if (!rpc->negotiated && request->kind != RequestCapabilities)
        return RingInvalid;
    switch (request->kind) {
    case RequestCapabilities:
        response->payload_bytes = VenusCapabilityBytes;
        return venus_receiver_capabilities(receiver, rpc->buffer, VenusCapabilityBytes);
    case RequestSubmit:
        return venus_receiver_submit(receiver, rpc->buffer, request->payload_bytes,
                                     &response->argument_zero);
    case RequestReply:
        response->payload_bytes = (uint32_t)request->argument_one;
        return venus_receiver_reply(receiver, request->argument_zero, rpc->buffer,
                                    response->payload_bytes);
    case RequestCreate:
        return venus_receiver_resource_create(receiver, request->resource_id,
                                              request->argument_zero, request->argument_one,
                                              request->flags);
    case RequestFree:
        return venus_receiver_resource_free(receiver, request->resource_id);
    case RequestRead:
        response->payload_bytes = (uint32_t)request->argument_one;
        return venus_receiver_resource_read(receiver, request->resource_id, request->argument_zero,
                                            rpc->buffer, response->payload_bytes);
    case RequestWrite:
        return venus_receiver_resource_write(receiver, request->resource_id, request->argument_zero,
                                             rpc->buffer, request->payload_bytes);
    case RequestGpuFence:
        return venus_receiver_gpu_fence(receiver, (uint32_t)request->argument_zero,
                                        &response->argument_zero);
    case RequestGpuPoll:
        return venus_receiver_gpu_poll(receiver, (uint32_t)request->argument_zero,
                                       request->argument_one);
    case RequestPresent:
    case RequestPresentPoll:
        return RingInvalid; /* Requires an explicitly trusted native presentation binding. */
    default:                /* Codec validation leaves only Poll. */
        return venus_receiver_poll(receiver);
    }
}

static venus_ring_status_t serve_request(venus_rpc_t *rpc, venus_receiver_t *receiver) {
    unsigned char header[VenusRequestHeaderBytes];
    venus_ring_status_t result = venus_rpc_transfer(rpc, header, sizeof(header), 0, 0);
    if (result != RingOk)
        return result;
    venus_request_t request;
    if (venus_request_decode(&request, header, sizeof(header)) != RingOk || request.direction ||
        request.sequence != rpc->next_sequence)
        return venus_rpc_fail(rpc, RingCorrupt);
    int limited = request.payload_bytes > rpc->buffer_bytes;
    if (limited) {
        unsigned char discarded[64];
        size_t remaining = request.payload_bytes;
        while (remaining) {
            size_t chunk = remaining > sizeof(discarded) ? sizeof(discarded) : remaining;
            result = venus_rpc_transfer(rpc, discarded, chunk, 0, 1);
            if (result != RingOk)
                return result;
            remaining -= chunk;
        }
    } else {
        result = venus_rpc_transfer(rpc, rpc->buffer, request.payload_bytes, 0, 1);
        if (result != RingOk)
            return result;
    }
    if (request.kind == RequestReply || request.kind == RequestRead)
        limited |= request.argument_one > rpc->buffer_bytes;
    venus_request_t response = {.kind = request.kind, .direction = 1, .sequence = request.sequence};
    result = limited ? RingLimit : dispatch(receiver, rpc, &request, &response);
    if (result != RingOk) {
        response.payload_bytes = 0;
        response.argument_zero = 0;
        switch (result) {
        case RingAgain:
            response.status = RequestAgain;
            break;
        case RingInvalid:
            response.status = RequestInvalid;
            break;
        case RingLimit:
            response.status = RequestLimit;
            break;
        default:
            return venus_rpc_fail(rpc, RingCorrupt);
        }
    }
    if (venus_request_encode(&response, header, sizeof(header)) != RingOk)
        return venus_rpc_fail(rpc, RingCorrupt);
    result = venus_rpc_transfer(rpc, header, sizeof(header), 1, 0);
    if (result == RingOk)
        result = venus_rpc_transfer(rpc, rpc->buffer, response.payload_bytes, 1, 1);
    if (result == RingOk)
        rpc->next_sequence++;
    return result;
}

/** @brief Call-scoped borrowed health state; never retained by the runtime. */
typedef struct runtime_health_t {
    venus_receiver_t *receiver;     /**< Borrowed live owner. */
    const _Atomic uint32_t *cancel; /**< Optional borrowed cancellation flag. */
} runtime_health_t;
static venus_ring_status_t check_health(void *context) {
    runtime_health_t *health = context;
    return venus_receiver_health(health->receiver, health->cancel);
}
venus_ring_status_t venus_dispatch_serve(venus_rpc_t *rpc, venus_receiver_t *receiver,
                                         uint32_t timeout_ms) {
    if (!receiver)
        return RingInvalid;
    venus_ring_status_t result = venus_rpc_begin(rpc, SessionHost, timeout_ms);
    if (result != RingOk)
        return result;
    runtime_health_t health = {.receiver = receiver, .cancel = rpc->channel->cancel};
    rpc->monitor = check_health;
    rpc->monitor_context = &health;
    result = serve_request(rpc, receiver);
    rpc->monitor = NULL;
    rpc->monitor_context = NULL;
    return result;
}
