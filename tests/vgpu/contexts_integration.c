/** @file contexts_integration.c @brief Simultaneous isolated Venus worker execution. */
#include "waddle/venus_capabilities.h"
#include "waddle/venus_context.h"
#include "waddle/venus_receiver.h"
#include "waddle/venus_rpc.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/** @brief Fixture-owned guest endpoint; worker never shares private pointers. */
typedef struct guest_context_t {
    void *mapping;            /**< Owned independent guest mapping view. */
    int mapping_fd;           /**< Owned original backing descriptor. */
    int stream_fd;            /**< Owned guest stream endpoint. */
    venus_session_t session;  /**< Local guest attachment. */
    venus_channel_t channel;  /**< Borrows local stream/session. */
    venus_rpc_t rpc;          /**< Borrows local private scratch. */
    unsigned char bytes[256]; /**< Exclusive bounded private transfer scratch. */
    uint64_t identity;        /**< Controller handle, never a renderer ID. */
} guest_context_t;

static unsigned descriptor_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory))
        count++;
    assert(!closedir(directory));
    return count;
}

static void exchange(guest_context_t *guest, venus_request_t request, const void *input,
                     void *output, size_t capacity, uint32_t expected) {
    venus_request_t response;
    assert(venus_rpc_exchange(&guest->rpc, &request, input, request.payload_bytes, &response,
                              output, capacity, 5000) == RingOk);
    assert(response.status == expected);
}
static void start_context(venus_context_manager_t *manager, guest_context_t *guest,
                          const char *path) {
    memset(guest, 0, sizeof(*guest));
    guest->mapping_fd = memfd_create("waddle_independent_context", MFD_CLOEXEC);
    assert(guest->mapping_fd >= 0 && !ftruncate(guest->mapping_fd, 4096));
    guest->mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, guest->mapping_fd, 0);
    assert(guest->mapping != MAP_FAILED);
    assert(venus_region_init(guest->mapping, 4096, 64) == RingOk);
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets));
    assert(venus_context_create(manager, path, guest->mapping_fd, sockets[0], &guest->identity) ==
           RingOk);
    close(sockets[0]);
    guest->stream_fd = sockets[1];
    assert(venus_session_init(&guest->session, SessionGuest, guest->mapping, 4096, 0) == RingOk);
    assert(venus_channel_init(&guest->channel, &guest->session, guest->stream_fd, NULL) == RingOk);
    assert(venus_channel_deadline(&guest->channel, 5000) == RingOk);
    assert(venus_channel_handshake(&guest->channel) == RingOk);
    assert(venus_rpc_init(&guest->rpc, &guest->channel, guest->bytes, sizeof(guest->bytes)) ==
           RingOk);
    /* A renderer request before negotiation must have no effect. */
    venus_request_t request = {
        .kind = RequestCreate, .resource_id = 2, .flags = ResourceMap, .argument_one = 4096};
    exchange(guest, request, NULL, NULL, 0, RequestInvalid);
    unsigned char capabilities[160];
    request = (venus_request_t){.kind = RequestCapabilities};
    exchange(guest, request, NULL, capabilities, sizeof(capabilities), RequestSuccess);
    venus_capabilities_t decoded;
    assert(venus_capabilities_decode(&decoded, capabilities, sizeof(capabilities)) == RingOk);
    assert(venus_capabilities_compatible(&decoded) == RingOk);
    request = (venus_request_t){.kind = RequestNegotiate, .payload_bytes = 160};
    unsigned char unsupported[160];
    memcpy(unsupported, capabilities, sizeof(unsupported));
    unsupported[0] = 2;
    exchange(guest, request, unsupported, NULL, 0, RequestInvalid);
    assert(!guest->rpc.negotiated);
    exchange(guest, request, capabilities, NULL, 0, RequestSuccess);
    assert(guest->rpc.negotiated);
    exchange(guest, request, capabilities, NULL, 0, RequestInvalid);
    request = (venus_request_t){
        .kind = RequestCreate, .resource_id = 2, .flags = ResourceMap, .argument_one = 4096};
    exchange(guest, request, NULL, NULL, 0, RequestSuccess);
    request = (venus_request_t){.kind = RequestWrite,
                                .resource_id = 2,
                                .payload_bytes = sizeof(guest->identity),
                                .argument_one = sizeof(guest->identity)};
    exchange(guest, request, &guest->identity, NULL, 0, RequestSuccess);
}
static void check_resource(guest_context_t *guest) {
    uint64_t contents = 0;
    venus_request_t request = {
        .kind = RequestRead, .resource_id = 2, .argument_one = sizeof(contents)};
    exchange(guest, request, NULL, &contents, sizeof(contents), RequestSuccess);
    assert(contents == guest->identity);
}
static void submit_version(guest_context_t *guest) {
    const uint32_t Commands[13] = {178, 0, 1, 0, 1, 0, 0, 4096, 0, 137, 1, 1, 0};
    venus_request_t request = {.kind = RequestSubmit, .payload_bytes = sizeof(Commands)}, response;
    assert(venus_rpc_exchange(&guest->rpc, &request, Commands, sizeof(Commands), &response, NULL, 0,
                              5000) == RingOk);
    assert(response.status == RequestSuccess && response.argument_zero == 1);
}
static void check_version(guest_context_t *guest) {
    venus_request_t request = {.kind = RequestPoll}, response;
    unsigned attempts = 0;
    do {
        assert(venus_rpc_exchange(&guest->rpc, &request, NULL, 0, &response, NULL, 0, 5000) ==
               RingOk);
        assert(++attempts < 10000);
    } while (response.status == RequestAgain);
    assert(response.status == RequestSuccess);
    uint32_t reply[5] = {0};
    request = (venus_request_t){.kind = RequestReply, .argument_one = sizeof(reply)};
    exchange(guest, request, NULL, reply, sizeof(reply), RequestSuccess);
    assert(reply[0] == 137 && reply[1] == 0 && reply[2] == 1 && reply[3] == 0 &&
           reply[4] >= (1u << 22));
}
static void free_guest(guest_context_t *guest) {
    assert(atomic_load(&guest->session.region.commands.header->flags) == VenusRingClosed);
    assert(atomic_load(&guest->session.region.replies.header->flags) == VenusRingClosed);
    venus_rpc_free(&guest->rpc);
    venus_channel_free(&guest->channel);
    venus_region_detach(&guest->session.region);
    assert(!munmap(guest->mapping, 4096));
    close(guest->mapping_fd);
    close(guest->stream_fd);
    memset(guest, 0, sizeof(*guest));
}
int main(void) {
    alarm(60);
    unsigned initial_descriptors = descriptor_count();
    char executable[PATH_MAX];
    const char *configured = getenv("WADDLE_PRODUCTION_WORKER");
    assert(realpath(configured ? configured : "build/waddle_vgpu_worker", executable));
    venus_context_manager_t manager = {0};
    assert(venus_context_manager_init(&manager, VenusContextMaxCount,
                                      4096 * VenusContextMaxCount) == RingOk);
    guest_context_t guests[VenusContextMaxCount];
    uint64_t previous = 0;
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned index = 0; index < VenusContextMaxCount; index++) {
            start_context(&manager, &guests[index], executable);
            assert(guests[index].identity > previous);
            previous = guests[index].identity;
            assert(venus_context_poll(&manager, guests[index].identity) == RingAgain);
        }
        assert(manager.live_count == VenusContextMaxCount && manager.live_bytes == 32768);
        /* All workers are live, with identical resource/CPU fence IDs. */
        for (unsigned index = 0; index < VenusContextMaxCount; index++)
            submit_version(&guests[index]);
        for (unsigned index = 0; index < VenusContextMaxCount; index++) {
            check_version(&guests[index]);
            check_resource(&guests[index]);
        }
        uint64_t stale = guests[0].identity;
        assert(!kill(manager.slots[0].worker.process_id, SIGKILL));
        venus_ring_status_t status;
        unsigned attempts = 0;
        do {
            status = venus_context_poll(&manager, stale);
            const struct timespec Pause = {0, 1000000};
            nanosleep(&Pause, NULL);
            assert(++attempts < 5000);
        } while (status == RingAgain);
        assert(status == RingClosed && manager.live_count == VenusContextMaxCount - 1);
        free_guest(&guests[0]);
        /* Crash one worker while the other seven retain separate contents. */
        for (unsigned index = 1; index < VenusContextMaxCount; index++)
            check_resource(&guests[index]);
        start_context(&manager, &guests[0], executable);
        assert(guests[0].identity > previous);
        previous = guests[0].identity;
        assert(venus_context_poll(&manager, stale) == RingInvalid);
        submit_version(&guests[0]);
        check_version(&guests[0]);
        for (unsigned index = 0; index < VenusContextMaxCount; index++) {
            check_resource(&guests[index]);
            assert(venus_context_destroy(&manager, guests[index].identity, 1000) == RingOk);
            free_guest(&guests[index]);
        }
        assert(!manager.live_count && !manager.live_bytes);
    }
    assert(venus_context_manager_free(&manager, 1000) == RingOk);
    assert(descriptor_count() == initial_descriptors);
    puts("Eight simultaneous Venus workers, profile retry, resource isolation, crash/restart and "
         "churn passed");
    return 0;
}
