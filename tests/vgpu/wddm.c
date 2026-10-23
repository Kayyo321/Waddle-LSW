/** @file wddm.c @brief Portable render-only stub identity and remote ownership faults. */
#include "waddle/venus_receiver.h"
#include "waddle/venus_wddm.h"
#include <assert.h>
#include <string.h>
static int calls;
static venus_ring_status_t first_status, second_status;
static uint32_t last_resource;
venus_ring_status_t venus_guest_exchange(venus_guest_t *guest, const venus_request_t *request,
                                         const void *input, size_t length,
                                         venus_request_t *response, void *output, size_t capacity) {
    assert(guest && guest->rpc && !input && !length && !output && !capacity);
    calls++;
    memset(response, 0, sizeof(*response));
    if (request->kind == RequestCreate) {
        assert(request->resource_id >= 2 && request->resource_id <= 65);
        assert(request->argument_zero && request->argument_one == 4096);
        last_resource = request->resource_id;
    } else if (request->kind == RequestGpuPoll) {
        assert(request->argument_zero == 1 && request->argument_one == 11);
    } else {
        assert(request->kind == RequestFree && request->resource_id >= 2 &&
               request->resource_id <= 65);
    }
    return calls == 1 ? first_status : second_status;
}
static void reset(void) {
    calls = 0;
    first_status = second_status = RingOk;
}
int main(void) {
    venus_wddm_t adapter = {0};
    venus_wddm_capabilities_t capabilities = {0};
    uint64_t handle = 123;
    venus_guest_t guests[9] = {0};
    venus_rpc_t rpcs[9] = {0};
    uint64_t contexts[8];
    for (unsigned index = 0; index < 9; index++) {
        rpcs[index].negotiated = 1;
        rpcs[index].next_sequence = 1;
        guests[index].rpc = &rpcs[index];
    }
    assert(venus_wddm_init(NULL, 1) == RingInvalid);
    assert(venus_wddm_init(&adapter, 0) == RingInvalid);
    assert(venus_wddm_capabilities(NULL, &capabilities) == RingInvalid);
    assert(venus_wddm_capabilities(&adapter, &capabilities) == RingInvalid);
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid && !handle);
    adapter.next_handle = 1;
    assert(venus_wddm_init(&adapter, 1) == RingInvalid);
    adapter.next_handle = 0;
    assert(venus_wddm_init(&adapter, 0x1234) == RingOk);
    assert(venus_wddm_init(&adapter, 1) == RingInvalid);
    assert(venus_wddm_capabilities(&adapter, NULL) == RingInvalid);
    assert(venus_wddm_capabilities(&adapter, &capabilities) == RingOk);
    assert(capabilities.adapter_luid == 0x1234 && capabilities.version == 1);
    assert(capabilities.flags == (AdapterRender | AdapterCompute));
    assert(capabilities.context_limit == 8 && capabilities.allocation_limit == 64);
    assert(venus_wddm_context_create(NULL, &guests[0], &handle) == RingInvalid);
    assert(venus_wddm_context_create(&adapter, NULL, &handle) == RingInvalid);
    assert(venus_wddm_context_create(&adapter, &guests[0], NULL) == RingInvalid);
    guests[0].rpc = NULL;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid);
    guests[0].rpc = &rpcs[0];
    rpcs[0].negotiated = 0;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid);
    rpcs[0].negotiated = 1;
    rpcs[0].next_sequence = 0;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid);
    rpcs[0].next_sequence = 1;
    guests[0].lost = RingClosed;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid);
    guests[0].lost = RingOk;
    adapter.next_handle = UINT64_MAX;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingLimit);
    adapter.next_handle = 1;
    for (unsigned index = 0; index < 8; index++)
        assert(venus_wddm_context_create(&adapter, &guests[index], &contexts[index]) == RingOk);
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingInvalid);
    venus_guest_t alias = guests[0];
    assert(venus_wddm_context_create(&adapter, &alias, &handle) == RingInvalid);
    assert(venus_wddm_context_create(&adapter, &guests[8], &handle) == RingLimit);
    assert(venus_wddm_free(&adapter) == RingAgain);
    assert(venus_wddm_context_destroy(NULL, contexts[0]) == RingInvalid);
    assert(venus_wddm_context_destroy(&adapter, 0) == RingInvalid);
    assert(venus_wddm_context_destroy(&adapter, UINT64_MAX) == RingInvalid);
    assert(venus_wddm_context_discard(&adapter, UINT64_MAX) == RingInvalid);
    assert(venus_wddm_context_discard(&adapter, contexts[0]) == RingInvalid);
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, ResourceShare, NULL) ==
           RingInvalid);
    assert(venus_wddm_allocation_create(NULL, contexts[0], 1, 4096, ResourceShare, &handle) ==
           RingInvalid);
    assert(venus_wddm_allocation_create(&adapter, UINT64_MAX, 1, 4096, ResourceShare, &handle) ==
           RingInvalid);
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 0, 4096, ResourceShare, &handle) ==
           RingInvalid);
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4095, ResourceShare, &handle) ==
           RingInvalid);
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, 8, &handle) == RingInvalid);
    uint64_t next_handle = adapter.next_handle;
    adapter.next_handle = UINT64_MAX;
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, ResourceShare, &handle) ==
           RingLimit);
    adapter.next_handle = next_handle;
    reset();
    first_status = RingLimit;
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, ResourceShare, &handle) ==
           RingLimit);
    assert(!handle && !adapter.contexts[0].allocations && adapter.next_handle == next_handle);
    uint64_t allocations[64];
    for (unsigned index = 0; index < 64; index++) {
        reset();
        assert(venus_wddm_allocation_create(&adapter, contexts[0], index + 1, 4096, ResourceShare,
                                            &allocations[index]) == RingOk);
        assert(last_resource == index + 2 && allocations[index] > contexts[7]);
    }
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, ResourceShare, &handle) ==
           RingLimit);
    assert(venus_wddm_context_destroy(&adapter, contexts[0]) == RingAgain);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], 0, 1, 11) == RingInvalid);
    assert(venus_wddm_allocation_destroy(&adapter, UINT64_MAX, allocations[0], 1, 11) ==
           RingInvalid);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 0, 11) ==
           RingInvalid);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 64, 11) ==
           RingInvalid);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 1, 0) ==
           RingInvalid);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[1], allocations[0], 1, 11) ==
           RingInvalid);
    reset();
    first_status = RingAgain;
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 1, 11) ==
               RingAgain &&
           calls == 1);
    reset();
    second_status = RingAgain;
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 1, 11) ==
               RingAgain &&
           calls == 2);
    assert(adapter.contexts[0].allocations == 64);
    reset();
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 1, 11) == RingOk);
    assert(venus_wddm_allocation_destroy(&adapter, contexts[0], allocations[0], 1, 11) ==
           RingInvalid);
    reset();
    assert(venus_wddm_allocation_create(&adapter, contexts[0], 1, 4096, ResourceShare, &handle) ==
           RingOk);
    assert(last_resource == 2 && handle > allocations[63]);
    guests[0].lost = RingClosed;
    reset();
    assert(venus_wddm_context_discard(&adapter, contexts[0]) == RingOk && !calls);
    assert(venus_wddm_context_destroy(&adapter, contexts[0]) == RingInvalid);
    guests[0].lost = RingOk;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingOk &&
           handle > allocations[63]);
    rpcs[0].next_sequence = 0;
    assert(venus_wddm_context_discard(&adapter, handle) == RingOk);
    rpcs[0].next_sequence = 1;
    assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingOk);
    guests[0].rpc = NULL;
    assert(venus_wddm_context_discard(&adapter, handle) == RingOk);
    guests[0].rpc = &rpcs[0];
    for (unsigned index = 1; index < 8; index++)
        assert(venus_wddm_context_destroy(&adapter, contexts[index]) == RingOk);
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        assert(venus_wddm_context_create(&adapter, &guests[0], &handle) == RingOk);
        assert(venus_wddm_context_destroy(&adapter, handle) == RingOk);
    }
    assert(venus_wddm_free(&adapter) == RingOk && !adapter.adapter_luid && !adapter.next_handle);
    assert(venus_wddm_free(&adapter) == RingOk);
    assert(venus_wddm_free(NULL) == RingOk);
    return 0;
}
