/** @file context.c @brief Context ledger faults, budgets, handles and ownership stress. */
#include "waddle/venus_context.h"
#include "waddle/venus_region.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fault, destroyed, spawned;
static venus_ring_status_t poll_result = RingAgain;
static pid_t failed_pid;
static void *fixture_map(void *address, size_t bytes, int protection, int flags, int fd,
                         off_t offset) {
    return fault == 1 ? MAP_FAILED : mmap(address, bytes, protection, flags, fd, offset);
}
static int fixture_dup(int fd, int command, int minimum) {
    assert(command == F_DUPFD_CLOEXEC);
    return fault == 2 ? -1 : fcntl(fd, command, minimum);
}
venus_ring_status_t venus_worker_create(venus_worker_t *worker, const char *path, int mapping,
                                        int stream) {
    assert(path && mapping >= 5);
    if (stream < 0)
        return RingInvalid;
    if (fault == 3)
        return RingCorrupt;
    *worker = (venus_worker_t){.process_id = ++spawned};
    return RingOk;
}
venus_ring_status_t venus_worker_poll(venus_worker_t *worker) {
    assert(worker->process_id);
    if (poll_result == RingClosed)
        worker->process_id = 0;
    return poll_result;
}
venus_ring_status_t venus_worker_destroy(venus_worker_t *worker, uint32_t timeout) {
    assert(timeout && timeout <= 60000 && worker->process_id);
    if (worker->process_id == failed_pid)
        return RingTimeout;
    destroyed++;
    memset(worker, 0, sizeof(*worker));
    return RingOk;
}
#define mmap fixture_map
#define fcntl fixture_dup
#include "venus_context.c"
#undef mmap
#undef fcntl

static int make_mapping(void **mapping) {
    int fd = memfd_create("waddle_context_fixture", MFD_CLOEXEC);
    assert(fd >= 0 && !ftruncate(fd, 4096));
    *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(*mapping != MAP_FAILED && venus_region_init(*mapping, 4096, 64) == RingOk);
    return fd;
}
int main(void) {
    void *mappings[9];
    int descriptors[9];
    for (unsigned index = 0; index < 9; index++)
        descriptors[index] = make_mapping(&mappings[index]);
    venus_context_manager_t manager = {0};
    uint64_t identity = 99;
    assert(venus_context_manager_init(NULL, 1, 4096) == RingInvalid);
    assert(venus_context_manager_init(&manager, 0, 4096) == RingInvalid);
    assert(venus_context_manager_init(&manager, 9, 4096) == RingInvalid);
    assert(venus_context_manager_init(&manager, 1, 4095) == RingInvalid);
    assert(venus_context_manager_init(&manager, 1, UINT64_C(8589934593)) == RingInvalid);
    assert(venus_context_manager_free(NULL, 0) == RingOk);
    assert(venus_context_manager_free(&manager, 0) == RingOk);
    assert(venus_context_create(NULL, "/worker", descriptors[0], 1, &identity) == RingInvalid);
    assert(!identity);
    assert(venus_context_poll(NULL, 1) == RingInvalid);
    assert(venus_context_destroy(&manager, 0, 1) == RingInvalid);
    assert(venus_context_manager_init(&manager, 8, 32768) == RingOk);
    assert(venus_context_manager_init(&manager, 8, 32768) == RingInvalid);
    assert(venus_context_create(&manager, "/worker", descriptors[0], 1, NULL) == RingInvalid);
    assert(venus_context_create(&manager, NULL, descriptors[0], 1, &identity) == RingInvalid);
    assert(venus_context_create(&manager, "worker", descriptors[0], 1, &identity) == RingInvalid);
    assert(venus_context_create(&manager, "/worker", -1, 1, &identity) == RingInvalid);
    assert(venus_context_create(&manager, "/worker", descriptors[0], -1, &identity) == RingInvalid);
    int device_fd = open("/dev/null", O_RDWR);
    assert(device_fd >= 0);
    assert(venus_context_create(&manager, "/worker", device_fd, 1, &identity) == RingInvalid);
    close(device_fd);
    for (int size_mode = 0; size_mode < 3; size_mode++) {
        off_t bytes = size_mode == 0 ? 4095 : size_mode == 1 ? 4097 : 1073741825;
        assert(!ftruncate(descriptors[0], bytes));
        assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) ==
               RingInvalid);
    }
    assert(!ftruncate(descriptors[0], 4096));
    for (fault = 1; fault <= 3; fault++) {
        assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) ==
               RingCorrupt);
        assert(!identity && !manager.live_count && !manager.live_bytes);
    }
    fault = 0;
    ((unsigned char *)mappings[0])[0] ^= 1;
    assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) == RingInvalid);
    ((unsigned char *)mappings[0])[0] ^= 1;
    venus_region_view_t view = {0};
    assert(venus_region_attach(&view, mappings[0], 4096) == RingOk);
    _Atomic uint32_t *fields[] = {&view.commands.header->head,  &view.commands.header->tail,
                                  &view.commands.header->flags, &view.replies.header->head,
                                  &view.replies.header->tail,   &view.replies.header->flags};
    for (unsigned index = 0; index < 6; index++) {
        atomic_store(fields[index], 1);
        assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) ==
               RingInvalid);
        atomic_store(fields[index], 0);
    }
    assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) == RingOk);
    uint64_t first = identity;
    assert(venus_context_poll(&manager, first) == RingAgain);
    assert(venus_context_poll(&manager, 0) == RingInvalid);
    assert(venus_context_poll(&manager, first + 1) == RingInvalid);
    assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) == RingInvalid);
    for (unsigned index = 1; index < 8; index++)
        assert(venus_context_create(&manager, "/worker", descriptors[index], 1, &identity) ==
               RingOk);
    assert(venus_context_create(&manager, "/worker", descriptors[8], 1, &identity) == RingLimit);
    assert(venus_context_destroy(&manager, first, 0) == RingInvalid);
    assert(venus_context_destroy(&manager, first, 60001) == RingInvalid);
    assert(venus_context_manager_free(&manager, 0) == RingInvalid);
    assert(venus_context_manager_free(&manager, 60001) == RingInvalid);
    failed_pid = manager.slots[0].worker.process_id;
    assert(venus_context_manager_free(&manager, 1000) == RingTimeout);
    assert(manager.live_count == 1 && manager.live_bytes == 4096);
    failed_pid = 0;
    int owned_fd = manager.slots[0].mapping_fd;
    assert(venus_context_destroy(&manager, first, 1000) == RingOk);
    assert(atomic_load(&view.commands.header->flags) == VenusRingClosed);
    assert(atomic_load(&view.replies.header->flags) == VenusRingClosed);
    assert(fcntl(owned_fd, F_GETFD) < 0 && errno == EBADF);
    assert(venus_context_destroy(&manager, first, 1000) == RingInvalid);
    assert(venus_context_manager_free(&manager, 1000) == RingOk);
    assert(!manager.count_limit);
    assert(venus_context_manager_init(&manager, 2, 4096) == RingOk);
    assert(venus_region_init(mappings[0], 4096, 64) == RingOk);
    assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) == RingOk);
    assert(venus_context_create(&manager, "/worker", descriptors[8], 1, &identity) == RingLimit);
    assert(venus_context_manager_free(&manager, 1000) == RingOk);
    assert(venus_context_manager_init(&manager, 2, 8192) == RingOk);
    manager.next_identity = UINT64_MAX;
    assert(venus_context_create(&manager, "/worker", descriptors[8], 1, &identity) == RingOk);
    assert(identity == UINT64_MAX);
    assert(venus_context_create(&manager, "/worker", descriptors[1], 1, &identity) == RingLimit);
    poll_result = RingCorrupt;
    assert(venus_context_poll(&manager, UINT64_MAX) == RingCorrupt && manager.live_count == 1);
    poll_result = RingClosed;
    fault = 1;
    assert(venus_context_poll(&manager, UINT64_MAX) == RingCorrupt && !manager.live_count);
    fault = 0;
    assert(venus_context_manager_free(&manager, 1000) == RingOk);
    assert(venus_context_manager_init(&manager, 2, 8192) == RingOk);
    uint64_t previous = 0;
    for (unsigned round = 0; round < 128; round++) {
        assert(venus_region_init(mappings[0], 4096, 64) == RingOk);
        assert(venus_context_create(&manager, "/worker", descriptors[0], 1, &identity) == RingOk);
        assert(identity > previous && venus_context_poll(&manager, previous) == RingInvalid);
        previous = identity;
        if (round == 0) {
            fault = 1;
            assert(venus_context_destroy(&manager, identity, 1000) == RingCorrupt);
            fault = 0;
        } else if (round == 1) {
            ((unsigned char *)mappings[0])[0] ^= 1;
            assert(venus_context_poll(&manager, identity) == RingClosed);
        } else
            assert(venus_context_poll(&manager, identity) == RingClosed);
        assert(!manager.live_count && !manager.live_bytes);
    }
    assert(venus_context_manager_free(&manager, 1000) == RingOk);
    for (unsigned index = 0; index < 9; index++) {
        munmap(mappings[index], 4096);
        close(descriptors[index]);
    }
    puts("Context limits, stale handles, all acquisition/shutdown faults and churn passed");
    return 0;
}
