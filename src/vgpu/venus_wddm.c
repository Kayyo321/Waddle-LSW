/** @file venus_wddm.c @brief Userland adapter/context/allocation identity lifecycle. */
#include "waddle/venus_wddm.h"
#include "venus_receiver_bounds.h"
#include <string.h>
static venus_wddm_context_t *find_context(venus_wddm_t *adapter, uint64_t handle) {
    if (!adapter || !adapter->adapter_luid || !handle)
        return NULL;
    for (unsigned index = 0; index < VenusWddmMaxContexts; index++)
        if (adapter->contexts[index].handle == handle)
            return &adapter->contexts[index];
    return NULL;
}
venus_ring_status_t venus_wddm_init(venus_wddm_t *adapter, uint64_t adapter_luid) {
    if (!adapter || adapter->adapter_luid || adapter->next_handle || !adapter_luid)
        return RingInvalid;
    adapter->adapter_luid = adapter_luid;
    adapter->next_handle = 1;
    return RingOk;
}
venus_ring_status_t venus_wddm_capabilities(const venus_wddm_t *adapter,
                                            venus_wddm_capabilities_t *capabilities) {
    if (!adapter || !adapter->adapter_luid || !capabilities)
        return RingInvalid;
    *capabilities = (venus_wddm_capabilities_t){.adapter_luid = adapter->adapter_luid,
                                                .version = 1,
                                                .flags = AdapterRender | AdapterCompute,
                                                .context_limit = VenusWddmMaxContexts,
                                                .allocation_limit = VenusWddmMaxAllocations};
    return RingOk;
}
venus_ring_status_t venus_wddm_context_create(venus_wddm_t *adapter, venus_guest_t *guest,
                                              uint64_t *output) {
    if (!output)
        return RingInvalid;
    *output = 0;
    if (!adapter || !adapter->adapter_luid || !guest || !guest->rpc || !guest->rpc->negotiated ||
        !guest->rpc->next_sequence || guest->lost)
        return RingInvalid;
    if (adapter->next_handle == UINT64_MAX)
        return RingLimit;
    venus_wddm_context_t *slot = NULL;
    for (unsigned index = 0; index < VenusWddmMaxContexts; index++) {
        venus_wddm_context_t *context = &adapter->contexts[index];
        if (context->handle) {
            if (context->guest == guest || context->guest->rpc == guest->rpc)
                return RingInvalid;
        } else if (!slot)
            slot = context;
    }
    if (!slot)
        return RingLimit;
    slot->handle = adapter->next_handle++;
    slot->guest = guest;
    *output = slot->handle;
    return RingOk;
}
venus_ring_status_t venus_wddm_context_destroy(venus_wddm_t *adapter, uint64_t handle) {
    venus_wddm_context_t *context = find_context(adapter, handle);
    if (!context)
        return RingInvalid;
    if (context->allocations)
        return RingAgain;
    memset(context, 0, sizeof(*context));
    return RingOk;
}
venus_ring_status_t venus_wddm_context_discard(venus_wddm_t *adapter, uint64_t handle) {
    venus_wddm_context_t *context = find_context(adapter, handle);
    if (!context ||
        (!context->guest->lost && context->guest->rpc && context->guest->rpc->next_sequence))
        return RingInvalid;
    memset(context, 0, sizeof(*context));
    return RingOk;
}
venus_ring_status_t venus_wddm_allocation_create(venus_wddm_t *adapter, uint64_t handle,
                                                 uint64_t blob, uint64_t bytes, uint32_t flags,
                                                 uint64_t *output) {
    if (!output)
        return RingInvalid;
    *output = 0;
    venus_wddm_context_t *context = find_context(adapter, handle);
    if (!context || !blob || !venus_receiver_resource_request(2, blob, bytes, flags))
        return RingInvalid;
    if (adapter->next_handle == UINT64_MAX || context->allocations == VenusWddmMaxAllocations)
        return RingLimit;
    unsigned index;
    for (index = 0; index < VenusWddmMaxAllocations; index++)
        if (!context->slots[index].handle)
            break;
    venus_request_t request = {.kind = RequestCreate,
                               .resource_id = index + 2,
                               .flags = flags,
                               .argument_zero = blob,
                               .argument_one = bytes},
                    response;
    venus_ring_status_t status =
        venus_guest_exchange(context->guest, &request, NULL, 0, &response, NULL, 0);
    if (status != RingOk)
        return status;
    context->slots[index] =
        (venus_wddm_allocation_t){.handle = adapter->next_handle++, .resource_id = index + 2};
    context->allocations++;
    *output = context->slots[index].handle;
    return RingOk;
}
venus_ring_status_t venus_wddm_allocation_destroy(venus_wddm_t *adapter, uint64_t handle,
                                                  uint64_t allocation, uint32_t timeline,
                                                  uint64_t fence) {
    venus_wddm_context_t *context = find_context(adapter, handle);
    if (!context || !allocation || !venus_receiver_gpu_timeline(timeline) || !fence)
        return RingInvalid;
    venus_wddm_allocation_t *slot = NULL;
    for (unsigned index = 0; index < VenusWddmMaxAllocations; index++)
        if (context->slots[index].handle == allocation) {
            slot = &context->slots[index];
            break;
        }
    if (!slot)
        return RingInvalid;
    venus_request_t request = {.kind = RequestGpuPoll,
                               .argument_zero = timeline,
                               .argument_one = fence},
                    response;
    venus_ring_status_t status =
        venus_guest_exchange(context->guest, &request, NULL, 0, &response, NULL, 0);
    if (status != RingOk)
        return status;
    request = (venus_request_t){.kind = RequestFree, .resource_id = slot->resource_id};
    status = venus_guest_exchange(context->guest, &request, NULL, 0, &response, NULL, 0);
    if (status != RingOk)
        return status;
    memset(slot, 0, sizeof(*slot));
    context->allocations--;
    return RingOk;
}
venus_ring_status_t venus_wddm_free(venus_wddm_t *adapter) {
    if (!adapter)
        return RingOk;
    for (unsigned index = 0; index < VenusWddmMaxContexts; index++)
        if (adapter->contexts[index].handle)
            return RingAgain;
    memset(adapter, 0, sizeof(*adapter));
    return RingOk;
}
