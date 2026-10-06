/** @file integration.c @brief Independent mapped processes and real Venus CPU dispatch. */
#include "waddle/venus_channel.h"
#include "waddle/venus_receiver.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Fixed fixture-only request length, one sequence plus thirteen wire words. */
#define RequestWords 14u
/** @brief Fixed fixture-only reply length, one sequence plus five actual Venus words. */
#define ReplyWords 6u
/** @brief Repeated independent-process transfers, enough to wrap both small rings. */
#define RoundTrips 32u
/** @brief Shared mock BAR extent, validated by production region helpers. */
#define MockMappingBytes 4096u
/** @brief Test modes; explicit peer exits exercise lifecycle EOF paths. */
typedef enum fixture_mode_t { FixtureNormal, FixtureDisconnect, FixturePartialEof } fixture_mode_t;

static int guest_run(void *inherited, int descriptor, int stream, fixture_mode_t mode) {
    int result = 1;
    void *mapping = mmap(NULL, MockMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    venus_session_t session = {0};
    venus_channel_t channel = {0};
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
        venus_channel_handshake(&channel) != RingOk)
        goto cleanup;
    for (uint32_t sequence = 1; sequence <= RoundTrips; sequence++) {
        /* Pinned Venus protocol: SetReplyCommandStreamMESA(resource one),
         * EnumerateInstanceVersion(GenerateReply), u64 pointer-present markers. */
        const uint32_t Request[RequestWords] = {sequence, 178,  0, 1,   0, 1, 0,
                                                0,        4096, 0, 137, 1, 1, 0};
        uint32_t reply[ReplyWords] = {0};
        if (venus_channel_deadline(&channel, 5000) != RingOk ||
            venus_ring_write_wait(&session.region.commands, Request, sizeof(Request),
                                  venus_channel_wait, &channel) != RingOk ||
            venus_ring_read_wait(&session.region.replies, reply, sizeof(reply), venus_channel_wait,
                                 &channel) != RingOk ||
            reply[0] != sequence || reply[1] != 137 || reply[2] != 0 || reply[3] != 1 ||
            reply[4] != 0 || reply[5] < (1u << 22))
            goto cleanup;
        if (mode != FixtureNormal) {
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
        venus_session_init(&session, SessionHost, mapping, MockMappingBytes, (uint64_t)guest) !=
            RingOk ||
        venus_channel_init(&channel, &session, sockets[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk)
        goto cleanup;
    for (uint32_t sequence = 1; sequence <= RoundTrips; sequence++) {
        uint32_t request[RequestWords] = {0};
        uint32_t reply[ReplyWords] = {sequence};
        if (venus_channel_deadline(&channel, 5000) != RingOk)
            goto cleanup;
        venus_ring_status_t status = venus_ring_read_wait(
            &session.region.commands, request, sizeof(request), venus_channel_wait, &channel);
        if (mode != FixtureNormal && sequence == 2) {
            if (status != (mode == FixtureDisconnect ? RingClosed : RingCorrupt))
                goto cleanup;
            result = 0;
            goto cleanup;
        }
        if (status != RingOk || request[0] != sequence)
            goto cleanup;
        uint64_t fence;
        if (venus_receiver_submit(receiver, request + 1, sizeof(request) - sizeof(request[0]),
                                  &fence) != RingOk ||
            fence != sequence)
            goto cleanup;
        while ((status = venus_receiver_poll(receiver)) == RingAgain)
            if (venus_channel_wait(&channel) != RingOk)
                goto cleanup;
        if (status != RingOk ||
            venus_receiver_reply(receiver, 0, reply + 1, sizeof(reply) - sizeof(reply[0])) !=
                RingOk ||
            venus_ring_write_wait(&session.region.replies, reply, sizeof(reply), venus_channel_wait,
                                  &channel) != RingOk)
            goto cleanup;
    }
    venus_ring_status_t status;
    do {
        status = venus_channel_wait(&channel);
    } while (status == RingOk);
    if (status != RingClosed || session.reason != StopDisconnect)
        goto cleanup;
    result = 0;
cleanup:
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
    for (int mode = FixtureNormal; mode <= FixturePartialEof; mode++) {
        if (run_fixture((fixture_mode_t)mode) != 0) {
            fprintf(stderr, "Independent mock Venus integration failed: mode=%d\n", mode);
            return 1;
        }
    }
    puts("Independent mapped-file/UNIX transport and real Venus replies passed");
    return 0;
}
