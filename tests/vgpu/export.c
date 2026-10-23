/** @file export.c @brief Worker export lease faults, ownership and churn. */
#include "waddle/venus_export.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned export_calls, fail_plane, send_calls, receives, queued_count;
static venus_ring_status_t export_status, send_status, receive_status;
static int prepare_status, extent_fault;
static venus_release_t queued[4];
static venus_receiver_t *const Receiver = (venus_receiver_t *)(uintptr_t)1;
venus_ring_status_t venus_frame_prepare(int fd) {
    assert(fd == 7);
    return prepare_status;
}
venus_ring_status_t venus_receiver_resource_export(venus_receiver_t *receiver, uint32_t id,
                                                   uint32_t timeline, uint64_t fence, int *fd) {
    assert(receiver == Receiver && id >= 2 && id <= 65 && timeline == 1 && fence == 1);
    export_calls++;
    *fd = -1;
    if (fail_plane == export_calls)
        return export_status;
    *fd = extent_fault == 1 ? open("/dev/null", O_RDONLY | O_CLOEXEC)
                            : memfd_create("export-fixture", MFD_CLOEXEC);
    assert(*fd >= 0);
    if (extent_fault != 1)
        assert(!ftruncate(*fd, extent_fault == 2 ? 2048 : 4096));
    return RingOk;
}
void venus_frame_fds_free(int fds[4]) {
    for (unsigned plane = 0; plane < 4; plane++) {
        if (fds[plane] >= 0)
            assert(!close(fds[plane]));
        fds[plane] = -1;
    }
}
venus_ring_status_t venus_frame_send(int fd, const venus_frame_t *frame, const int *fds,
                                     size_t count) {
    assert(fd == 7 && frame->context == 17 && count == frame->layout.plane_count);
    for (size_t plane = 0; plane < count; plane++)
        assert(fcntl(fds[plane], F_GETFD) & FD_CLOEXEC);
    send_calls++;
    return send_status;
}
venus_ring_status_t venus_release_receive(int fd, int32_t pid, uint64_t context,
                                          venus_release_t *release) {
    assert(fd == 7 && pid == 23 && context == 17);
    receives++;
    if (queued_count) {
        *release = queued[0];
        memmove(queued, queued + 1, --queued_count * sizeof(*queued));
        return RingOk;
    }
    return receive_status;
}
static off_t fixture_lseek(int fd, off_t offset, int origin) {
    return extent_fault == 3 ? -1 : lseek(fd, offset, origin);
}
#define lseek fixture_lseek
#include "venus_export.c"
#undef lseek
static unsigned descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory))
        count++;
    assert(!closedir(directory));
    return count;
}
static void reset(venus_export_t *owner) {
    venus_export_free(owner);
    prepare_status = RingOk;
    export_calls = fail_plane = send_calls = receives = queued_count = 0;
    export_status = send_status = RingOk;
    receive_status = RingAgain;
    extent_fault = 0;
    assert(venus_export_init(owner, Receiver, 7, 23, 17) == RingOk);
}
static venus_frame_t example(uint64_t id, uint32_t resource) {
    return (venus_frame_t){.context = 17,
                           .frame = id,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {resource},
                           .damage_count = 1,
                           .damage = {{.width = 32, .height = 16}}};
}
static venus_ring_status_t submit(venus_export_t *owner, venus_frame_t frame) {
    unsigned char bytes[VenusFrameBytes];
    assert(venus_frame_encode(&frame, bytes, sizeof(bytes)) == RingOk);
    return venus_export_submit(owner, bytes, sizeof(bytes), 1, 1);
}
static void acknowledge(uint64_t id, venus_ring_status_t status) {
    assert(queued_count < 4);
    queued[queued_count++] = (venus_release_t){.context = 17, .frame = id, .status = status};
}
int main(void) {
    unsigned baseline = descriptors();
    venus_export_t owner = {0}, empty = {0};
    venus_release_t release;
    assert(venus_export_init(NULL, Receiver, 7, 23, 17) == RingInvalid);
    assert(venus_export_init(&owner, NULL, 7, 23, 17) == RingInvalid);
    assert(venus_export_init(&owner, Receiver, 7, 0, 17) == RingInvalid);
    assert(venus_export_init(&owner, Receiver, 7, 23, 0) == RingInvalid);
    prepare_status = RingInvalid;
    assert(venus_export_init(&owner, Receiver, 7, 23, 17) == RingInvalid);
    reset(&owner);
    assert(venus_export_init(&owner, Receiver, 7, 23, 17) == RingInvalid);
    assert(venus_export_submit(NULL, NULL, 0, 1, 1) == RingInvalid);
    assert(venus_export_submit(&empty, NULL, 0, 1, 1) == RingInvalid);
    assert(venus_export_submit(&owner, NULL, 0, 0, 1) == RingInvalid);
    assert(venus_export_submit(&owner, NULL, 0, 64, 1) == RingInvalid);
    assert(venus_export_submit(&owner, NULL, 0, 1, 0) == RingInvalid);
    assert(venus_export_submit(&owner, NULL, 0, 1, 1) == RingInvalid);
    assert(venus_export_pump(NULL) == RingInvalid);
    assert(venus_export_pump(&empty) == RingInvalid);
    assert(venus_export_take(&owner, 1, NULL) == RingInvalid);
    assert(venus_export_take(&owner, 0, &release) == RingInvalid && !release.frame);
    assert(venus_export_take(&owner, 1, &release) == RingInvalid);
    assert(!venus_export_resource_busy(NULL, 2));
    assert(!venus_export_resource_busy(&empty, 2));
    assert(!venus_export_resource_busy(&owner, 1));
    assert(!venus_export_resource_busy(&owner, 66));
    venus_frame_t frame = example(1, 2);
    frame.context = 18;
    assert(submit(&owner, frame) == RingInvalid);
    frame.context = 17;
    for (extent_fault = 1; extent_fault <= 3; extent_fault++) {
        assert(submit(&owner, frame) == RingInvalid);
        assert(!owner.last_frame && descriptors() == baseline);
    }
    extent_fault = 0;
    const venus_ring_status_t Errors[] = {RingAgain,   RingInvalid,   RingLimit,  RingClosed,
                                          RingCorrupt, RingCancelled, RingTimeout};
    for (unsigned index = 0; index < sizeof(Errors) / sizeof(*Errors); index++) {
        reset(&owner);
        fail_plane = 1;
        export_status = Errors[index];
        assert(submit(&owner, frame) == Errors[index]);
        assert(!owner.last_frame && !venus_export_resource_busy(&owner, 2));
        assert(descriptors() == baseline);
        if (owner.terminal != RingOk) {
            assert(submit(&owner, frame) == owner.terminal);
            assert(venus_export_take(&owner, 1, &release) == owner.terminal);
        }
        reset(&owner);
        send_status = Errors[index];
        assert(submit(&owner, frame) == Errors[index]);
        assert(!owner.last_frame && descriptors() == baseline);
    }
    reset(&owner);
    frame.layout = (venus_dmabuf_layout_t){
        .width = 32,
        .height = 16,
        .fourcc = 0x3231564e,
        .plane_count = 2,
        .planes = {{.stride = 32, .size = 512, .extent = 4096},
                   {.offset = 512, .stride = 32, .size = 256, .extent = 4096}}};
    frame.resource_ids[1] = 2; /* One allocation can contain both planes. */
    fail_plane = 2;
    export_status = RingAgain;
    assert(submit(&owner, frame) == RingAgain && export_calls == 2 && !send_calls);
    assert(descriptors() == baseline);
    fail_plane = 0;
    assert(submit(&owner, frame) == RingOk && send_calls == 1 && owner.last_frame == 1);
    assert(descriptors() == baseline && venus_export_resource_busy(&owner, 2));
    assert(submit(&owner, frame) == RingInvalid);
    assert(venus_export_take(&owner, 1, &release) == RingAgain && !release.frame);
    frame = example(2, 2);
    assert(submit(&owner, frame) == RingAgain);
    frame.resource_ids[0] = 3;
    assert(submit(&owner, frame) == RingOk);
    assert(submit(&owner, example(3, 4)) == RingOk);
    assert(submit(&owner, example(4, 5)) == RingAgain);
    assert(!venus_export_resource_busy(&owner, 65));
    acknowledge(2, RingInvalid);
    acknowledge(1, RingOk);
    acknowledge(3, RingCancelled);
    assert(venus_export_pump(&owner) == RingOk && receives >= 3 && !queued_count);
    assert(venus_export_resource_busy(&owner, 2));
    assert(submit(&owner, example(4, 5)) == RingAgain);
    assert(venus_export_take(&owner, 2, &release) == RingOk && release.status == RingInvalid);
    assert(venus_export_take(&owner, 2, &release) == RingInvalid && !release.frame);
    assert(!venus_export_resource_busy(&owner, 3));
    assert(venus_export_take(&owner, 1, &release) == RingOk && release.status == RingOk);
    assert(venus_export_take(&owner, 3, &release) == RingOk && release.status == RingCancelled);
    assert(submit(&owner, example(4, 5)) == RingOk);
    acknowledge(4, RingClosed);
    assert(venus_export_take(&owner, 4, &release) == RingOk && release.status == RingClosed);
    for (unsigned mode = 0; mode < 4; mode++) {
        reset(&owner);
        assert(submit(&owner, example(1, 2)) == RingOk);
        if (mode < 2) {
            acknowledge(mode ? 1 : 2, RingOk);
            if (mode) {
                assert(venus_export_pump(&owner) == RingOk);
                acknowledge(1, RingOk);
            }
        } else
            receive_status = mode == 2 ? RingClosed : RingCorrupt;
        venus_ring_status_t expected = mode == 2 ? RingClosed : RingCorrupt;
        assert(venus_export_pump(&owner) == expected);
        unsigned calls = receives;
        assert(venus_export_pump(&owner) == expected && receives == calls);
        assert(venus_export_resource_busy(&owner, 2));
    }
    reset(&owner);
    for (unsigned iteration = 1; iteration <= 128; iteration++) {
        assert(submit(&owner, example(iteration, 2)) == RingOk);
        acknowledge(iteration, RingOk);
        assert(venus_export_take(&owner, iteration, &release) == RingOk);
        assert(!venus_export_resource_busy(&owner, 2) && descriptors() == baseline);
    }
    venus_export_free(NULL);
    venus_export_free(&owner);
    venus_export_free(&owner);
    assert(!owner.receiver && !owner.last_frame && descriptors() == baseline);
    return 0;
}
