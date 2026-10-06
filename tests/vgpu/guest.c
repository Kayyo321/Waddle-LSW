/** @file guest.c @brief Native negotiated frontend status and lifetime faults. */
#include "waddle/venus_guest.h"
#include <assert.h>
#include <string.h>
static venus_rpc_t rpc;
static venus_channel_t channel;
static venus_session_t session;
static int calls, frees, fault_step, bad_shape, bad_version;
static uint32_t wire_status, expected_timeout;
static venus_ring_status_t exchange_status;
static unsigned char profile[160] = {[0] = 1,  [4] = 0x33, [5] = 0x41, [6] = 0x40, [8] = 1,
                                     [12] = 3, [16] = 1,   [20] = 1,   [68] = 3,   [152] = 1};
venus_ring_status_t venus_rpc_exchange(venus_rpc_t *record, const venus_request_t *request,
                                       const void *input, size_t length, venus_request_t *response,
                                       void *output, size_t capacity, uint32_t timeout) {
    assert(record == &rpc && timeout == expected_timeout);
    calls++;
    if (calls == fault_step && exchange_status != RingOk)
        return exchange_status;
    *response = (venus_request_t){.status = calls == fault_step ? wire_status : RequestSuccess,
                                  .argument_zero = 17};
    if (request->kind == RequestCapabilities) {
        assert(!input && !length && capacity == 160);
        memcpy(output, profile, 160);
        if (bad_shape)
            ((unsigned char *)output)[16] = 2;
        if (bad_version)
            ((unsigned char *)output)[0] = 2;
    } else if (request->kind == RequestNegotiate) {
        assert(length == 160 && !output && !capacity);
        assert(!memcmp(input, profile, 160));
        if (!response->status)
            rpc.negotiated = 1;
    } else {
        assert(request->kind == RequestSubmit);
        assert(length == 8 && capacity == 4);
        if (!response->status)
            memset(output, 0xa5, capacity);
    }
    return RingOk;
}
void venus_rpc_free(venus_rpc_t *record) {
    assert(record == &rpc);
    frees++;
    memset(record, 0, sizeof(*record));
}
static void reset(void) {
    calls = frees = fault_step = bad_shape = bad_version = 0;
    expected_timeout = 100;
    wire_status = RequestSuccess;
    exchange_status = RingOk;
    session = (venus_session_t){.role = SessionGuest, .state = SessionReady};
    channel = (venus_channel_t){.session = &session};
    rpc = (venus_rpc_t){.channel = &channel, .next_sequence = 1};
}
static void test_init(void) {
    venus_guest_t guest = {0};
    reset();
    assert(venus_guest_init(NULL, &rpc, 100) == RingInvalid);
    assert(venus_guest_init(&guest, NULL, 100) == RingInvalid);
    guest.rpc = &rpc;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    guest.rpc = NULL;
    guest.timeout_ms = 1;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    guest.timeout_ms = 0;
    guest.lost = RingCorrupt;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    guest.lost = RingOk;
    rpc.channel = NULL;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    rpc.channel = &channel;
    channel.session = NULL;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    channel.session = &session;
    session.role = SessionHost;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    session.role = SessionGuest;
    session.state = SessionClosed;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    session.state = SessionReady;
    rpc.negotiated = 1;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    rpc.negotiated = 0;
    rpc.next_sequence = 0;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid);
    rpc.next_sequence = 1;
    assert(venus_guest_init(&guest, &rpc, 0) == RingInvalid);
    assert(venus_guest_init(&guest, &rpc, 60001) == RingInvalid);
    assert(!calls);
    for (int step = 1; step <= 2; step++) {
        reset();
        fault_step = step;
        exchange_status = RingClosed;
        assert(venus_guest_init(&guest, &rpc, 100) == RingClosed && !guest.rpc);
        reset();
        fault_step = step;
        wire_status = RequestInvalid;
        assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid && !guest.rpc);
    }
    reset();
    bad_shape = 1;
    assert(venus_guest_init(&guest, &rpc, 100) == RingCorrupt && !guest.rpc);
    reset();
    bad_version = 1;
    assert(venus_guest_init(&guest, &rpc, 100) == RingInvalid && !guest.rpc);
    reset();
    assert(venus_guest_init(&guest, &rpc, 100) == RingOk && calls == 2 && rpc.negotiated);
    assert(guest.capabilities.vk_xml_version == VenusPinnedXmlVersion);
    venus_guest_free(&guest);
    assert(!guest.rpc && !guest.timeout_ms && frees == 1);
    venus_guest_free(&guest);
    venus_guest_free(NULL);
    assert(frees == 1);
}
static void test_exchange_timeout(void) {
    const venus_ring_status_t Statuses[] = {RingOk, RingAgain, RingInvalid, RingCorrupt,
                                            RingClosed, RingCancelled, RingTimeout, RingLimit};
    venus_guest_t guest = {0};
    venus_request_t request = {.kind = RequestSubmit, .payload_bytes = 8}, response;
    unsigned char bytes[8] = {0}, output[4];
    reset();
    assert(venus_guest_exchange_timeout(NULL,&request,bytes,8,NULL,output,4,1)==RingInvalid);
    assert(venus_guest_exchange_timeout(NULL,&request,bytes,8,&response,output,4,1)==RingInvalid);
    assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,1)==RingInvalid);
    assert(!calls && !frees);
    assert(venus_guest_init(&guest,&rpc,100)==RingOk);
    for (unsigned index=0;index<3;++index) {
        const uint32_t Invalid[] = {0,101,60001};
        memset(&response,0xa5,sizeof response); memset(output,0x5a,sizeof output);
        assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,Invalid[index])==RingInvalid);
        venus_request_t zero = {0}; assert(!memcmp(&response,&zero,sizeof response));
        assert(calls==2 && !frees && guest.timeout_ms==100 && guest.lost==RingOk && output[0]==0x5a);
    }
    assert(venus_guest_exchange_timeout(&guest,NULL,bytes,8,&response,output,4,1)==RingInvalid);
    request.kind=RequestCapabilities;
    assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,1)==RingInvalid);
    request.kind=RequestNegotiate;
    assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,1)==RingInvalid);
    request.kind=RequestSubmit;
    assert(calls==2);
    for (uint32_t timeout=1;timeout<=100;timeout+=99) {
        expected_timeout=timeout;
        assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,timeout)==RingOk);
        assert(guest.timeout_ms==100 && response.argument_zero==17 && output[0]==0xa5);
    }
    expected_timeout=100;
    assert(venus_guest_exchange(&guest,&request,bytes,8,&response,output,4)==RingOk);
    venus_guest_free(&guest);
    for (uint32_t status=0;status<8;++status) for (unsigned transport=0;transport<2;++transport) {
        reset(); assert(venus_guest_init(&guest,&rpc,100)==RingOk);
        expected_timeout=37; fault_step=3;
        if (transport) exchange_status=Statuses[status]; else wire_status=status;
        memset(output,0x5a,sizeof output);
        assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,37)==Statuses[status]);
        assert(calls==3 && guest.timeout_ms==100);
        int terminal=status>=3 && status<=6;
        assert(frees==terminal);
        if (terminal) {
            assert(guest.lost==Statuses[status]);
            assert(venus_guest_exchange_timeout(&guest,&request,bytes,8,&response,output,4,0)==Statuses[status]);
            assert(calls==3 && frees==1 && output[0]==0x5a);
        } else assert(guest.lost==RingOk);
        venus_guest_free(&guest);
    }
}
int main(void) {
    test_init();
    test_exchange_timeout();
    const venus_ring_status_t Statuses[] = {RingOk,     RingAgain,     RingInvalid, RingCorrupt,
                                            RingClosed, RingCancelled, RingTimeout, RingLimit};
    venus_request_t request = {.kind = RequestSubmit, .payload_bytes = 8}, response;
    unsigned char bytes[8] = {0}, output[4];
    venus_guest_t guest = {0};
    assert(venus_guest_exchange(NULL, &request, bytes, 8, NULL, output, 4) == RingInvalid);
    assert(venus_guest_exchange(NULL, &request, bytes, 8, &response, output, 4) == RingInvalid);
    assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) == RingInvalid);
    for (uint32_t wire = 0; wire <= 8; wire++) {
        reset();
        assert(venus_guest_init(&guest, &rpc, 100) == RingOk);
        assert(venus_guest_exchange(&guest, NULL, bytes, 8, &response, output, 4) == RingInvalid);
        request.kind = RequestCapabilities;
        assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) ==
               RingInvalid);
        request.kind = RequestNegotiate;
        assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) ==
               RingInvalid);
        request.kind = RequestSubmit;
        fault_step = 3;
        wire_status = wire;
        memset(output, 0x5a, sizeof(output));
        venus_ring_status_t expected = wire < 8 ? Statuses[wire] : RingCorrupt;
        assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) == expected);
        if (expected == RingOk) {
            assert(response.argument_zero == 17 && output[0] == 0xa5);
        } else {
            assert(output[0] == 0x5a);
        }
        if (expected < RingInvalid && expected != RingLimit) {
            assert(guest.lost == expected && frees == 1);
            assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) ==
                   expected);
            assert(calls == 3);
        } else {
            assert(!guest.lost && !frees);
            assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) ==
                   RingOk);
        }
        venus_guest_free(&guest);
    }
    for (unsigned index = 1; index < 8; index++) {
        reset();
        assert(venus_guest_init(&guest, &rpc, 100) == RingOk);
        fault_step = 3;
        exchange_status = Statuses[index];
        assert(venus_guest_exchange(&guest, &request, bytes, 8, &response, output, 4) ==
               Statuses[index]);
        venus_guest_free(&guest);
    }
    for (int iteration = 0; iteration < 128; iteration++) {
        reset();
        assert(venus_guest_init(&guest, &rpc, 100) == RingOk);
        venus_guest_free(&guest);
    }
    return 0;
}
