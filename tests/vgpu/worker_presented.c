/** @file worker_presented.c @brief Real presented worker exec, negotiation and channel isolation.
 */
#include "waddle/venus_frame.h"
#include "waddle/venus_guest.h"
#include "waddle/venus_worker.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory)
        return 0;
    unsigned count = 0;
    while (readdir(directory))
        count++;
    if (closedir(directory))
        return 0;
    return count;
}
static int run_fixture(int corrupt) {
    int result = 1, mapping_fd = -1, streams[2] = {-1, -1}, frames[2] = {-1, -1};
    void *mapping = MAP_FAILED;
    venus_worker_t worker = {0};
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    venus_guest_t guest = {0};
    unsigned char scratch[4096], bytes[VenusFrameBytes], completion[VenusReleaseBytes];
    char executable[PATH_MAX];
    const char *configured = getenv("WADDLE_PRODUCTION_WORKER");
    if (!realpath(configured ? configured : "build/waddle_vgpu_worker", executable))
        goto cleanup;
    mapping_fd = memfd_create("presented-worker", MFD_CLOEXEC);
    if (mapping_fd < 0 || ftruncate(mapping_fd, 4096) != 0)
        goto cleanup;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, 0);
    if (mapping == MAP_FAILED || venus_region_init(mapping, 4096, 64) != RingOk ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, streams) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, frames) != 0 ||
        venus_frame_prepare(frames[0]) != RingOk || venus_frame_prepare(frames[1]) != RingOk)
        goto cleanup;
    if (venus_worker_create_presented(&worker, executable, mapping_fd, streams[1], frames[1],
                                      UINT64_MAX) != RingOk)
        goto cleanup;
    close(streams[1]);
    streams[1] = -1;
    close(frames[1]);
    frames[1] = -1;
    if (venus_session_init(&session, SessionGuest, mapping, 4096, 0) != RingOk ||
        venus_channel_init(&channel, &session, streams[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) != RingOk ||
        venus_guest_init(&guest, &rpc, 5000) != RingOk)
        goto cleanup;
    venus_frame_t frame = {.context = UINT64_MAX,
                           .frame = 1,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.width = 32, .height = 16}}};
    venus_request_t request = {.kind = RequestPresent,
                               .payload_bytes = sizeof(bytes),
                               .argument_zero = 1,
                               .argument_one = 1},
                    response;
    if (venus_frame_encode(&frame, bytes, sizeof(bytes)) != RingOk ||
        venus_guest_exchange(&guest, &request, bytes, sizeof(bytes), &response, NULL, 0) !=
            RingInvalid)
        goto cleanup; /* No registered allocation/fence: never publish a frame. */
    request = (venus_request_t){.kind = RequestPresentPoll, .argument_zero = 1};
    if (venus_guest_exchange(&guest, &request, NULL, 0, &response, completion,
                             sizeof(completion)) != RingInvalid)
        goto cleanup;
    int received[4];
    if (venus_frame_receive(frames[0], worker.process_id, UINT64_MAX, &frame, received) !=
        RingAgain)
        goto cleanup;
    if (corrupt) {
        const venus_release_t Unknown = {.context = UINT64_MAX, .frame = 1, .status = RingOk};
        if (venus_release_send(frames[0], &Unknown) != RingOk)
            goto cleanup;
        request = (venus_request_t){.kind = RequestPoll};
        venus_ring_status_t status =
            venus_guest_exchange(&guest, &request, NULL, 0, &response, NULL, 0);
        if (status != RingClosed && status != RingCorrupt)
            goto cleanup;
    }
    venus_guest_free(&guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    close(streams[0]);
    streams[0] = -1;
    for (unsigned attempt = 0;; attempt++) {
        venus_ring_status_t status = venus_worker_poll(&worker);
        if (status == RingClosed) {
            if (!WIFEXITED(worker.exit_status) || WEXITSTATUS(worker.exit_status) != corrupt)
                goto cleanup;
            break;
        }
        if (status != RingAgain || attempt >= 5000)
            goto cleanup;
        usleep(1000);
    }
    result = 0;
cleanup:
    venus_guest_free(&guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    if (venus_worker_destroy(&worker, 1000) != RingOk)
        result = 1;
    for (unsigned index = 0; index < 2; index++) {
        if (streams[index] >= 0)
            close(streams[index]);
        if (frames[index] >= 0)
            close(frames[index]);
    }
    venus_region_detach(&session.region);
    if (mapping != MAP_FAILED)
        munmap(mapping, 4096);
    if (mapping_fd >= 0)
        close(mapping_fd);
    return result;
}
int main(void) {
    alarm(20);
    unsigned baseline = descriptors();
    if (!baseline)
        return 1;
    for (unsigned corrupt = 0; corrupt < 2; corrupt++)
        if (run_fixture((int)corrupt) || descriptors() != baseline) {
            fprintf(stderr, "Presented worker binding failed: mode=%u\n", corrupt);
            return 1;
        }
    puts("Presented production worker negotiation, isolation and unknown-release shutdown passed");
    return 0;
}
