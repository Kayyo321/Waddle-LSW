/** @file integration.c @brief Independent mapped processes and real Venus CPU dispatch. */
#include "waddle/venus_channel.h"
#include "waddle/venus_dispatch.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Repeated independent-process transfers, enough to wrap both small rings. */
#define RoundTrips 32u
/** @brief Shared mock BAR extent, validated by production region helpers. */
#define MockMappingBytes 4096u
/** @brief Test modes; explicit peer exits exercise lifecycle EOF paths. */
typedef enum fixture_mode_t {
    FixtureNormal,
    FixtureDisconnect,
    FixturePartialEof,
    FixtureBadSequence,
    FixtureBadHeader,
    FixturePartialRequest
} fixture_mode_t;

static int guest_exchange(venus_rpc_t *rpc, venus_request_t request, const void *input,
                          void *output, size_t output_bytes, uint32_t status) {
    venus_request_t response;
    return venus_rpc_exchange(rpc, &request, input, request.payload_bytes, &response, output,
                              output_bytes, 5000) == RingOk &&
           response.status == status;
}

static int guest_resources(venus_rpc_t *rpc) {
    unsigned char capabilities[VenusCapabilityBytes], input[300], output[300];
    for (size_t index = 0; index < sizeof(input); index++)
        input[index] = (unsigned char)index;
    venus_request_t request = {.kind = RequestCapabilities};
    if (!guest_exchange(rpc, request, NULL, capabilities, sizeof(capabilities), RequestSuccess))
        return 0;
    request = (venus_request_t){
        .kind = RequestCreate, .resource_id = 2, .flags = ResourceMap, .argument_one = 4096};
    if (!guest_exchange(rpc, request, NULL, NULL, 0, RequestSuccess) ||
        !guest_exchange(rpc, request, NULL, NULL, 0, RequestInvalid))
        return 0;
    request.resource_id = 3;
    if (!guest_exchange(rpc, request, NULL, NULL, 0, RequestLimit))
        return 0;
    request = (venus_request_t){.kind = RequestWrite,
                                .resource_id = 2,
                                .argument_zero = 3796,
                                .argument_one = sizeof(input),
                                .payload_bytes = sizeof(input)};
    if (!guest_exchange(rpc, request, input, NULL, 0, RequestSuccess))
        return 0;
    request.kind = RequestRead;
    request.payload_bytes = 0;
    if (!guest_exchange(rpc, request, NULL, output, sizeof(output), RequestSuccess) ||
        memcmp(input, output, sizeof(input)))
        return 0;
    request.argument_zero++;
    if (!guest_exchange(rpc, request, NULL, output, sizeof(output), RequestInvalid))
        return 0;
    request = (venus_request_t){.kind = RequestFree, .resource_id = 2};
    if (!guest_exchange(rpc, request, NULL, NULL, 0, RequestSuccess))
        return 0;
    request = (venus_request_t){
        .kind = RequestCreate, .resource_id = 65, .flags = ResourceMap, .argument_one = 4096};
    /* Keep final registered resource live to test owner cleanup after peer exit. */
    return guest_exchange(rpc, request, NULL, NULL, 0, RequestSuccess);
}

static int guest_run(void *inherited, int descriptor, int stream, fixture_mode_t mode) {
    int result = 1;
    void *mapping = mmap(NULL, MockMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    unsigned char private_bytes[4096];
    if (mapping == MAP_FAILED)
        goto cleanup;
    /* Remapping while the inherited address remains occupied proves endpoints
     * share offsets/bytes, rather than coincidentally sharing pointer values. */
    if (mapping == inherited)
        goto cleanup;
    if (munmap(inherited, MockMappingBytes) != 0)
        goto cleanup;
    inherited = NULL;
    if (venus_session_init(&session, SessionGuest, mapping, MockMappingBytes, 0) != RingOk ||
        venus_channel_init(&channel, &session, stream, NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, private_bytes, sizeof(private_bytes)) != RingOk)
        goto cleanup;
    if (!guest_resources(&rpc))
        goto cleanup;
    for (uint32_t sequence = 1; sequence <= RoundTrips; sequence++) {
        /* Pinned Venus stream/resource one, real instance-version command. */
        const uint32_t Commands[13] = {178, 0, 1, 0, 1, 0, 0, 4096, 0, 137, 1, 1, 0};
        uint32_t reply[5] = {0};
        venus_request_t request = {.kind = RequestSubmit, .payload_bytes = sizeof(Commands)};
        venus_request_t response;
        if (venus_rpc_exchange(&rpc, &request, Commands, sizeof(Commands), &response, NULL, 0,
                               5000) != RingOk ||
            response.status != RequestSuccess || response.argument_zero != sequence)
            goto cleanup;
        request = (venus_request_t){.kind = RequestPoll};
        do {
            if (venus_rpc_exchange(&rpc, &request, NULL, 0, &response, NULL, 0, 5000) != RingOk)
                goto cleanup;
        } while (response.status == RequestAgain);
        if (response.status != RequestSuccess)
            goto cleanup;
        request = (venus_request_t){.kind = RequestReply, .argument_one = sizeof(reply)};
        if (!guest_exchange(&rpc, request, NULL, reply, sizeof(reply), RequestSuccess) ||
            reply[0] != 137 || reply[1] != 0 || reply[2] != 1 || reply[3] != 0 ||
            reply[4] < (1u << 22))
            goto cleanup;
        if (mode != FixtureNormal) {
            if (mode == FixtureBadSequence || mode == FixtureBadHeader ||
                mode == FixturePartialRequest) {
                unsigned char frame[VenusRequestHeaderBytes];
                venus_request_t invalid = {.kind = RequestPoll, .sequence = rpc.next_sequence};
                if (mode == FixtureBadSequence)
                    invalid.sequence++;
                if (mode == FixturePartialRequest) {
                    invalid.kind = RequestWrite;
                    invalid.resource_id = 65;
                    invalid.argument_one = invalid.payload_bytes = 300;
                }
                if (venus_request_encode(&invalid, frame, sizeof(frame)) != RingOk)
                    goto cleanup;
                if (mode == FixtureBadHeader)
                    frame[0] ^= 1;
                if (venus_channel_deadline(&channel, 5000) != RingOk ||
                    venus_ring_write_wait(&session.region.commands, frame, sizeof(frame),
                                          venus_channel_wait, &channel) != RingOk)
                    goto cleanup;
                if (mode == FixturePartialRequest) {
                    if (venus_ring_write_wait(&session.region.commands, frame, 7,
                                              venus_channel_wait, &channel) != RingOk)
                        goto cleanup;
                } else {
                    venus_ring_status_t terminal;
                    do {
                        terminal = venus_channel_wait(&channel);
                    } while (terminal == RingOk);
                    if (terminal != RingClosed)
                        goto cleanup;
                }
            }
            if (mode == FixturePartialEof) {
                uint8_t frame[VenusControlBytes];
                const venus_control_t Stop = {.kind = ControlStop,
                                              .reason = StopDisconnect,
                                              .session_id = session.session_id,
                                              .mapping_bytes = MockMappingBytes,
                                              .capacity = 64};
                if (venus_control_encode(&Stop, frame, sizeof(frame)) != RingOk ||
                    send(stream, frame, 7, MSG_NOSIGNAL) != 7)
                    goto cleanup;
            }
            /* Simulated process crash: leave shared flags open and omit Stop.
             * Linux channel owns no event/heap; kernel closes the peer fd below. */
            memset(&rpc, 0, sizeof(rpc));
            memset(&channel, 0, sizeof(channel));
            result = 0;
            goto cleanup;
        }
    }
    if (venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_stop(&channel, StopDisconnect) != RingOk)
        goto cleanup;
    result = 0;
cleanup:
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    venus_region_detach(&session.region);
    if (mapping != MAP_FAILED)
        munmap(mapping, MockMappingBytes);
    if (inherited)
        munmap(inherited, MockMappingBytes);
    close(descriptor);
    close(stream);
    return result;
}

static int run_fixture(fixture_mode_t mode) {
    int result = 1, descriptor = -1, linked = 0, sockets[2] = {-1, -1};
    pid_t guest = -1;
    void *mapping = MAP_FAILED;
    venus_receiver_t *receiver = NULL;
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    unsigned char private_bytes[4096];
    char path[] = "/dev/shm/waddle_test_XXXXXX";
    descriptor = mkstemp(path);
    if (descriptor < 0)
        goto cleanup;
    linked = 1;
    if (unlink(path) != 0)
        goto cleanup;
    linked = 0;
    if (ftruncate(descriptor, MockMappingBytes) != 0)
        goto cleanup;
    mapping = mmap(NULL, MockMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED || venus_region_init(mapping, MockMappingBytes, 64) != RingOk ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) != 0)
        goto cleanup;
    /* No renderer threads exist at fork; the guest performs only its local
     * mapping/session/socket operations, and renderer ownership stays host-side. */
    guest = fork();
    if (guest < 0)
        goto cleanup;
    if (guest == 0) {
        close(sockets[0]);
        alarm(30);
        exit(guest_run(mapping, descriptor, sockets[1], mode));
    }
    close(sockets[1]);
    sockets[1] = -1;
    if (venus_receiver_create(&receiver, 64, 4096) != RingOk ||
        venus_receiver_resource_limits(receiver, 1, 4096) != RingOk ||
        venus_session_init(&session, SessionHost, mapping, MockMappingBytes, (uint64_t)guest) !=
            RingOk ||
        venus_channel_init(&channel, &session, sockets[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, private_bytes, sizeof(private_bytes)) != RingOk)
        goto cleanup;
    venus_ring_status_t status;
    do {
        status = venus_dispatch_serve(&rpc, receiver, 5000);
    } while (status == RingOk);
    int corrupt = mode == FixturePartialEof || mode == FixtureBadSequence ||
                  mode == FixtureBadHeader || mode == FixturePartialRequest;
    int protocol_reason =
        mode == FixturePartialEof || mode == FixtureBadSequence || mode == FixtureBadHeader;
    if (status != (corrupt ? RingCorrupt : RingClosed) ||
        session.reason != (protocol_reason ? StopProtocol : StopDisconnect))
        goto cleanup;
    result = 0;
cleanup:
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    venus_receiver_destroy(&receiver); /* Joins all callbacks before mapping release. */
    venus_region_detach(&session.region);
    if (sockets[0] >= 0)
        close(sockets[0]);
    if (sockets[1] >= 0)
        close(sockets[1]);
    if (guest > 0) {
        int status = 0;
        if (result)
            kill(guest, SIGTERM); /* Only the child created by this fixture. */
        if (waitpid(guest, &status, 0) != guest || !WIFEXITED(status) || WEXITSTATUS(status))
            result = 1;
    }
    if (mapping != MAP_FAILED)
        munmap(mapping, MockMappingBytes);
    if (descriptor >= 0)
        close(descriptor);
    if (linked)
        unlink(path);
    return result;
}

int main(void) {
    alarm(30);
    for (int mode = FixtureNormal; mode <= FixturePartialRequest; mode++) {
        if (run_fixture((fixture_mode_t)mode) != 0) {
            fprintf(stderr, "Independent mock Venus integration failed: mode=%d\n", mode);
            return 1;
        }
    }
    puts("Independent mapped runtime, resource quotas and real Venus replies passed");
    return 0;
}
