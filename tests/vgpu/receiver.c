/** @file receiver.c @brief Real public-ABI Venus dispatch and repeated ownership. */
#include "waddle/venus_receiver.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int wait_reply(venus_receiver_t *receiver) {
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        return -1;
    for (;;) {
        venus_ring_status_t status = venus_receiver_poll(receiver);
        if (status != RingAgain)
            return status == RingOk ? 0 : -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec - start.tv_sec >= 5)
            return -1;
        const struct timespec Pause = {0, 1000000};
        nanosleep(&Pause, NULL);
    }
}

int main(void) {
    /* Exact pinned protocol: SetReplyCommandStreamMESA then
     * EnumerateInstanceVersion (GenerateReply), u64 simple-pointer markers.
     * Res ID one is the receiver's reply blob. No device/queue is created. */
    const uint32_t Commands[] = {178, 0, 1, 0, 1, 0, 0, 4096, 0, 137, 1, 1, 0};
    int result = 1;
    venus_receiver_t *receiver = NULL;
    for (int iteration = 0; iteration < 3; iteration++) {
        if (venus_receiver_create(&receiver, 64, 4096) != RingOk)
            goto cleanup;
        uint8_t capabilities[VenusCapabilityBytes];
        if (venus_receiver_capabilities(receiver, capabilities, sizeof(capabilities)) != RingOk)
            goto cleanup;
        uint32_t blob_support;
        memcpy(&blob_support, capabilities + 16, sizeof(blob_support));
        if (!blob_support)
            goto cleanup;
        const uint8_t Input[] = {1, 2, 3, 4};
        uint8_t output[4] = {0};
        if (venus_receiver_resource_limits(receiver, 1, 4096) != RingOk ||
            venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) != RingOk ||
            venus_receiver_resource_create(receiver, 3, 0, 4096, ResourceMap) != RingLimit ||
            venus_receiver_resource_write(receiver, 2, 4092, Input, sizeof(Input)) != RingOk ||
            venus_receiver_resource_read(receiver, 2, 4092, output, sizeof(output)) != RingOk ||
            memcmp(output, Input, sizeof(Input)) != 0 ||
            venus_receiver_resource_free(receiver, 2) != RingOk ||
            venus_receiver_resource_create(receiver, 65, 0, 4096, ResourceMap) != RingOk ||
            venus_receiver_resource_read(receiver, 65, 0, output, sizeof(output)) != RingOk)
            goto cleanup;
        /* Resource 65 is mapped and released implicitly by each owner teardown. */
        for (int submission = 0; submission < 3; submission++) {
            uint64_t fence = 0;
            if (venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) != RingOk ||
                fence != (uint64_t)submission + 1 || wait_reply(receiver) != 0)
                goto cleanup;
            uint32_t reply[5] = {0};
            if (venus_receiver_reply(receiver, 0, reply, sizeof(reply)) != RingOk ||
                reply[0] != 137 || reply[1] != 0 || reply[2] != 1 || reply[3] != 0 ||
                reply[4] < (1u << 22))
                goto cleanup;
            printf("Real Venus CPU dispatch: fence=%llu Vulkan API=0x%08x\n",
                   (unsigned long long)fence, reply[4]);
        }
        venus_receiver_destroy(&receiver);
        if (receiver)
            goto cleanup;
    }
    if (venus_receiver_create(&receiver, 64, 4096) != RingOk)
        goto cleanup;
    uint64_t fence;
    if (venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) != RingOk)
        goto cleanup;
    /* Teardown with a fence potentially pending must join callbacks. */
    result = 0;
cleanup:
    venus_receiver_destroy(&receiver);
    venus_receiver_destroy(&receiver);
    if (result)
        fputs("Real Venus receiver bootstrap/dispatch failed\n", stderr);
    return result;
}
