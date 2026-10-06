/** @file surface.c @brief Worker/surface binding acquisition, FD retry and identity faults. */
#include "waddle/venus_surface.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int allocation_fail, source_fd, received_fd, receive_calls, submit_calls;
static venus_ring_status_t create_status, receive_status, submit_status, free_status;
static venus_present_done_t completion;
static void *completion_context;
static venus_frame_t next_frame, last_completed;
static venus_ring_status_t last_status;
static unsigned completed_count;
static void *allocate(size_t count, size_t size) {
    return allocation_fail ? NULL : calloc(count, size);
}
#define calloc allocate
#include "venus_surface.c"
#undef calloc
venus_ring_status_t venus_frame_prepare(int fd) { return fd >= 0 ? RingOk : RingInvalid; }
venus_ring_status_t venus_present_create(venus_present_t **output, struct wl_display *display,
                                         struct zwp_linux_dmabuf_v1 *dmabuf,
                                         struct wl_surface *surface, venus_present_done_t done,
                                         void *context) {
    if (!display || !dmabuf || !surface)
        return RingInvalid;
    if (create_status != RingOk)
        return create_status;
    completion = done;
    completion_context = context;
    *output = (venus_present_t *)&completion;
    return RingOk;
}
venus_ring_status_t venus_present_submit(venus_present_t *presenter, uint64_t frame,
                                         const venus_dmabuf_layout_t *layout, const int *fds,
                                         size_t count, const venus_dmabuf_damage_t *damage,
                                         size_t damage_count) {
    assert(presenter && frame && layout && damage && damage_count == 1 && count == 1);
    assert(fcntl(fds[0], F_GETFD) >= 0);
    submit_calls++;
    return submit_status;
}
venus_ring_status_t venus_present_free(venus_present_t **presenter) {
    if (free_status == RingOk)
        *presenter = NULL;
    return free_status;
}
venus_ring_status_t venus_frame_receive(int fd, int32_t pid, uint64_t identity,
                                        venus_frame_t *frame, int fds[4]) {
    assert(fd >= 0 && pid == 123 && identity == 456);
    receive_calls++;
    if (receive_status != RingOk)
        return receive_status;
    *frame = next_frame;
    fds[0] = received_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    assert(fds[0] >= 0);
    return RingOk;
}
void venus_frame_fds_free(int fds[4]) {
    for (size_t index = 0; index < 4; index++) {
        if (fds[index] >= 0)
            close(fds[index]);
        fds[index] = -1;
    }
}
static venus_ring_status_t release_status;
static unsigned release_calls, release_successes, release_after;
static venus_release_t last_release;
venus_ring_status_t venus_release_send(int fd, const venus_release_t *release) {
    assert(fd == source_fd && release && release->context == 456 && release->frame);
    release_calls++;
    venus_ring_status_t status =
        release_after && release_successes >= release_after ? RingAgain : release_status;
    if (status == RingOk) {
        last_release = *release;
        release_successes++;
    }
    return status;
}
static void completed(void *context, const venus_frame_t *frame, venus_ring_status_t status) {
    assert(context == &completed_count && frame->context == 456 && frame->resource_ids[0] == 2);
    completed_count++;
    last_completed = *frame;
    last_status = status;
}
static venus_ring_status_t create_owner(venus_surface_t **owner) {
    return venus_surface_create(owner, source_fd, 123, 456, (struct wl_display *)&completion,
                                (struct zwp_linux_dmabuf_v1 *)&completion,
                                (struct wl_surface *)&completion, completed, &completed_count);
}
static void expect_closed(void) { assert(fcntl(received_fd, F_GETFD) == -1); }
static void acknowledged_queue(void) {
    venus_surface_t *owner = NULL;
    create_status = receive_status = submit_status = free_status = RingOk;
    release_status = RingOk;
    release_after = release_calls = release_successes = 0;
    assert(venus_surface_create_acknowledged(NULL, source_fd, 123, 456, NULL, NULL, NULL, NULL,
                                             NULL) == RingInvalid);
    assert(venus_surface_create_acknowledged(
               &owner, source_fd, 123, 456, (struct wl_display *)&completion,
               (struct zwp_linux_dmabuf_v1 *)&completion, (struct wl_surface *)&completion,
               completed, &completed_count) == RingOk);
    next_frame = (venus_frame_t){.context = 456,
                                 .frame = 1,
                                 .resource_ids = {2},
                                 .layout = {.plane_count = 1},
                                 .damage_count = 1};
    for (unsigned id = 1; id <= 3; id++) {
        next_frame.frame = id;
        assert(venus_surface_poll(owner) == RingOk);
    }
    completion(completion_context, 2, RingInvalid);
    completion(completion_context, 1, RingOk);
    completion(completion_context, 3, RingClosed);
    release_status = RingAgain;
    unsigned received = receive_calls;
    assert(venus_surface_poll(owner) == RingAgain && receive_calls == (int)received);
    assert(venus_surface_free(&owner) == RingAgain && owner);
    release_status = RingOk;
    release_after = 1;
    assert(venus_surface_poll(owner) == RingAgain && release_successes == 1);
    assert(last_release.frame == 2 && last_release.status == RingInvalid);
    release_after = 0;
    assert(venus_surface_poll(owner) == RingClosed && release_successes == 3);
    assert(last_release.frame == 3 && last_release.status == RingClosed);
    assert(venus_surface_free(&owner) == RingOk && !owner);
    assert(venus_surface_create_acknowledged(
               &owner, source_fd, 123, 456, (struct wl_display *)&completion,
               (struct zwp_linux_dmabuf_v1 *)&completion, (struct wl_surface *)&completion, NULL,
               NULL) == RingOk);
    next_frame.frame = 1;
    submit_status = RingAgain;
    assert(venus_surface_poll(owner) == RingAgain);
    assert(venus_surface_cancel_pending(owner) == RingOk);
    release_status = RingAgain;
    assert(venus_surface_poll(owner) == RingAgain);
    release_status = RingOk;
    receive_status = RingAgain;
    assert(venus_surface_poll(owner) == RingAgain);
    assert(last_release.frame == 1 && last_release.status == RingInvalid);
    next_frame.frame = 2;
    receive_status = RingOk;
    assert(venus_surface_poll(owner) == RingAgain);
    release_status = RingAgain;
    assert(venus_surface_free(&owner) == RingAgain && owner && !owner->presenter);
    release_status = RingOk;
    assert(venus_surface_free(&owner) == RingOk && !owner);
    assert(last_release.frame == 2 && last_release.status == RingCancelled);
    const venus_ring_status_t Faults[] = {RingClosed, RingCorrupt, RingInvalid};
    for (unsigned fault = 0; fault < 3; fault++) {
        release_status = RingOk;
        assert(venus_surface_create_acknowledged(
                   &owner, source_fd, 123, 456, (struct wl_display *)&completion,
                   (struct zwp_linux_dmabuf_v1 *)&completion, (struct wl_surface *)&completion,
                   NULL, NULL) == RingOk);
        acknowledged_done(owner, &next_frame, RingOk);
        release_status = Faults[fault];
        venus_ring_status_t expected = fault ? RingCorrupt : RingClosed;
        assert(venus_surface_poll(owner) == expected);
        unsigned sent = release_calls;
        release_status = RingOk;
        assert(venus_surface_poll(owner) == expected && release_calls == sent);
        free_status = RingAgain;
        assert(venus_surface_free(&owner) == RingAgain && owner);
        free_status = RingOk;
        assert(venus_surface_free(&owner) == expected && !owner);
    }
    release_status = RingOk;
    assert(venus_surface_create_acknowledged(
               &owner, source_fd, 123, 456, (struct wl_display *)&completion,
               (struct zwp_linux_dmabuf_v1 *)&completion, (struct wl_surface *)&completion, NULL,
               NULL) == RingOk);
    acknowledged_done(owner, &next_frame, RingCorrupt);
    assert(venus_surface_poll(owner) == RingCorrupt);
    for (unsigned id = 1; id <= 5; id++) {
        next_frame.frame = id;
        acknowledged_done(owner, &next_frame, RingOk);
    }
    assert(owner->failed == RingCorrupt);
    assert(venus_surface_free(&owner) == RingOk && !owner);
}
int main(void) {
    source_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(source_fd >= 0);
    venus_surface_t *owner = NULL;
    assert(venus_surface_poll(NULL) == RingInvalid);
    assert(venus_surface_cancel_pending(NULL) == RingInvalid);
    assert(venus_surface_free(NULL) == RingOk && venus_surface_free(&owner) == RingOk);
    assert(venus_surface_create(NULL, source_fd, 123, 456, NULL, NULL, NULL, completed, NULL) ==
           RingInvalid);
    assert(venus_surface_create(&owner, source_fd, 0, 456, NULL, NULL, NULL, completed, NULL) ==
           RingInvalid);
    assert(venus_surface_create(&owner, source_fd, 123, 0, NULL, NULL, NULL, completed, NULL) ==
           RingInvalid);
    assert(venus_surface_create(&owner, source_fd, 123, 456, NULL, NULL, NULL, NULL, NULL) ==
           RingInvalid);
    assert(venus_surface_create(&owner, -1, 123, 456, NULL, NULL, NULL, completed, NULL) ==
           RingInvalid);
    allocation_fail = 1;
    assert(create_owner(&owner) == RingCorrupt && !owner);
    allocation_fail = 0;
    create_status = RingCorrupt;
    assert(create_owner(&owner) == RingCorrupt && !owner);
    create_status = RingOk;
    assert(create_owner(&owner) == RingOk && owner);
    assert(create_owner(&owner) == RingInvalid);
    assert(venus_surface_cancel_pending(owner) == RingAgain);
    next_frame = (venus_frame_t){.context = 456,
                                 .frame = 1,
                                 .layout = {.plane_count = 1},
                                 .resource_ids = {2},
                                 .damage_count = 1};
    receive_status = RingAgain;
    assert(venus_surface_poll(owner) == RingAgain);
    receive_status = RingOk;
    submit_status = RingAgain;
    assert(venus_surface_poll(owner) == RingAgain);
    int calls = receive_calls;
    assert(fcntl(received_fd, F_GETFD) >= 0);
    assert(venus_surface_poll(owner) == RingAgain && calls == receive_calls);
    assert(venus_surface_cancel_pending(owner) == RingOk && last_status == RingInvalid);
    expect_closed();
    next_frame.frame++;
    assert(venus_surface_poll(owner) == RingAgain);
    free_status = RingAgain;
    assert(venus_surface_free(&owner) == RingAgain && owner);
    assert(fcntl(received_fd, F_GETFD) >= 0);
    free_status = RingOk;
    assert(venus_surface_free(&owner) == RingOk && !owner && last_status == RingCancelled);
    expect_closed();
    assert(create_owner(&owner) == RingOk);
    submit_status = RingInvalid;
    assert(venus_surface_poll(owner) == RingInvalid && last_status == RingInvalid);
    expect_closed();
    submit_status = RingOk;
    for (unsigned index = 0; index < 3; index++) {
        next_frame.frame++;
        assert(venus_surface_poll(owner) == RingOk);
        expect_closed();
    }
    calls = receive_calls;
    assert(venus_surface_poll(owner) == RingAgain && calls == receive_calls);
    completion(completion_context, next_frame.frame - 1, RingOk);
    assert(last_completed.frame == next_frame.frame - 1 && last_status == RingOk);
    completion(completion_context, next_frame.frame, RingInvalid);
    completion(completion_context, next_frame.frame - 2, RingOk);
    assert(venus_surface_poll(owner) == RingCorrupt); // Last identity cannot be reused.
    expect_closed();
    calls = receive_calls;
    assert(venus_surface_poll(owner) == RingCorrupt && calls == receive_calls);
    assert(venus_surface_free(&owner) == RingOk);
    for (unsigned index = 0; index < 128; index++) {
        assert(create_owner(&owner) == RingOk);
        next_frame.frame++;
        assert(venus_surface_poll(owner) == RingOk);
        completion(completion_context, next_frame.frame, RingOk);
        assert(venus_surface_free(&owner) == RingOk && !owner);
    }
    assert(create_owner(&owner) == RingOk);
    receive_status = RingClosed;
    assert(venus_surface_poll(owner) == RingClosed);
    assert(venus_surface_poll(owner) == RingClosed);
    assert(venus_surface_free(&owner) == RingOk);
    receive_status = RingOk;
    assert(create_owner(&owner) == RingOk);
    submit_status = RingCorrupt;
    next_frame.frame++;
    assert(venus_surface_poll(owner) == RingCorrupt && last_status == RingCorrupt);
    expect_closed();
    assert(venus_surface_poll(owner) == RingCorrupt);
    assert(venus_surface_free(&owner) == RingOk);
    submit_status = RingOk;
    for (unsigned index = 0; index < 2; index++) {
        assert(create_owner(&owner) == RingOk);
        next_frame.frame++;
        assert(venus_surface_poll(owner) == RingOk);
        completion(completion_context, next_frame.frame, index ? RingCorrupt : RingClosed);
        assert(venus_surface_poll(owner) == (index ? RingCorrupt : RingClosed));
        assert(venus_surface_free(&owner) == RingOk);
    }
    assert(create_owner(&owner) == RingOk);
    unsigned saved_count = completed_count;
    completion(completion_context, 0, RingOk); // Empty slots cannot match a zero identity.
    assert(completed_count == saved_count);
    assert(venus_surface_poll(owner) == RingCorrupt);
    assert(venus_surface_free(&owner) == RingOk);
    assert(create_owner(&owner) == RingOk);
    completion(completion_context, UINT64_MAX, RingOk); // Internal identity violation.
    assert(venus_surface_poll(owner) == RingCorrupt);
    assert(venus_surface_free(&owner) == RingOk);
    assert(submit_calls > 128 && completed_count > 128);
    acknowledged_queue();
    close(source_fd);
    return 0;
}
