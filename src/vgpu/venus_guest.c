/** @file venus_guest.c @brief Pinned profile and serialized guest exchange ownership. */
#include "waddle/venus_guest.h"
#include <string.h>

/** @brief Constant little-endian pinned declaration, mandatory extensions only. */
static const unsigned char GuestProfile[VenusCapabilitiesBytes] = {
    [0] = 1,  [4] = 0x33, [5] = 0x41, [6] = 0x40, [8] = 1,
    [12] = 3, [16] = 1,   [20] = 1,   [68] = 3,   [152] = 1};

static venus_ring_status_t operation_status(uint32_t status) {
    switch (status) {
    case RequestSuccess:
        return RingOk;
    case RequestAgain:
        return RingAgain;
    case RequestInvalid:
        return RingInvalid;
    case RequestCorrupt:
        return RingCorrupt;
    case RequestClosed:
        return RingClosed;
    case RequestCancelled:
        return RingCancelled;
    case RequestTimeout:
        return RingTimeout;
    case RequestLimit:
        return RingLimit;
    default:
        return RingCorrupt;
    }
}

venus_ring_status_t venus_guest_init(venus_guest_t *guest, venus_rpc_t *rpc, uint32_t timeout_ms) {
    if (!guest || guest->rpc || guest->timeout_ms || guest->lost || !rpc || !rpc->channel ||
        !rpc->channel->session || rpc->channel->session->role != SessionGuest ||
        rpc->channel->session->state != SessionReady || rpc->negotiated || !rpc->next_sequence ||
        !timeout_ms || timeout_ms > 60000)
        return RingInvalid;
    unsigned char bytes[VenusCapabilitiesBytes];
    venus_request_t request = {.kind = RequestCapabilities}, response;
    venus_ring_status_t status =
        venus_rpc_exchange(rpc, &request, NULL, 0, &response, bytes, sizeof(bytes), timeout_ms);
    if (status != RingOk)
        return status;
    status = operation_status(response.status);
    if (status != RingOk)
        return status;
    venus_capabilities_t capabilities;
    status = venus_capabilities_decode(&capabilities, bytes, sizeof(bytes));
    if (status != RingOk)
        return status;
    status = venus_capabilities_compatible(&capabilities);
    if (status != RingOk)
        return status;
    request = (venus_request_t){.kind = RequestNegotiate, .payload_bytes = sizeof(GuestProfile)};
    status = venus_rpc_exchange(rpc, &request, GuestProfile, sizeof(GuestProfile), &response, NULL,
                                0, timeout_ms);
    if (status != RingOk)
        return status;
    status = operation_status(response.status);
    if (status != RingOk)
        return status;
    *guest = (venus_guest_t){.rpc = rpc, .capabilities = capabilities, .timeout_ms = timeout_ms};
    return RingOk;
}

static venus_ring_status_t exchange_once(venus_guest_t *guest, const venus_request_t *request,
                                         const void *input, size_t length,
                                         venus_request_t *response, void *output, size_t capacity,
                                         uint64_t deadline_ms, uint32_t timeout_ms, int absolute) {
    if (!response)
        return RingInvalid;
    memset(response, 0, sizeof(*response));
    if (!guest || !guest->rpc || !request)
        return RingInvalid;
    if (guest->lost != RingOk)
        return guest->lost;
    if (absolute ? !deadline_ms :
        (!timeout_ms || timeout_ms > guest->timeout_ms || timeout_ms > 60000))
        return RingInvalid;
    if (request->kind == RequestCapabilities || request->kind == RequestNegotiate)
        return RingInvalid;
    venus_ring_status_t status = absolute
        ? venus_rpc_exchange_until(guest->rpc, request, input, length, response, output,
                                   capacity, deadline_ms, guest->timeout_ms)
        : venus_rpc_exchange(guest->rpc, request, input, length, response,
                              output, capacity, timeout_ms);
    if (status == RingOk)
        status = operation_status(response->status);
    if (status == RingCorrupt || status == RingClosed || status == RingCancelled ||
        status == RingTimeout) {
        guest->lost = status;
        venus_rpc_free(guest->rpc);
    }
    return status;
}

venus_ring_status_t venus_guest_exchange_timeout(venus_guest_t *guest, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity,
    uint32_t timeout_ms) {
    return exchange_once(guest, request, input, length, response, output, capacity, 0, timeout_ms, 0);
}

venus_ring_status_t venus_guest_exchange_until(venus_guest_t *guest, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity,
    uint64_t deadline_ms) {
    return exchange_once(guest, request, input, length, response, output, capacity, deadline_ms, 0, 1);
}

venus_ring_status_t venus_guest_exchange(venus_guest_t *guest, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity) {
    return venus_guest_exchange_timeout(guest, request, input, length, response, output, capacity,
                                       guest ? guest->timeout_ms : 0);
}

void venus_guest_free(venus_guest_t *guest) {
    if (!guest)
        return;
    if (guest->rpc)
        venus_rpc_free(guest->rpc);
    memset(guest, 0, sizeof(*guest));
}
