/** @file receiver_owner.c @brief Public renderer boundary acquisition fault fixture. */
#include "waddle/venus_receiver.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <venus_hw.h>
#include <virglrenderer.h>

static uint64_t clock_ms = 1000;
static int clock_mode;
static int fixture_clock_gettime(clockid_t clock, struct timespec *now) {
    assert(clock == CLOCK_MONOTONIC);
    if (clock_mode == 1)
        return -1;
    now->tv_sec = (time_t)(clock_ms / 1000);
    now->tv_nsec = (long)(clock_ms % 1000) * 1000000;
    if (clock_mode == 2)
        now->tv_sec = -1;
    if (clock_mode == 3)
        now->tv_nsec = -1;
    if (clock_mode == 4)
        now->tv_nsec = 1000000000;
    if (clock_mode == 5)
        now->tv_sec = (time_t)(UINT64_MAX / 1000 + 1);
    return 0;
}

static int fault;
static int fence_inline;
static int allocations;
static int active_renderer;
static int active_context;
static int active_resource;
static int active_map;
static void *callback_cookie;
static struct virgl_renderer_callbacks *callback_table;
static _Alignas(64) uint8_t reply_memory[4096];
static int additional_live[VenusReceiverMaxResources];
static void *additional_maps[VenusReceiverMaxResources];
static uint64_t additional_bytes[VenusReceiverMaxResources];
static uint64_t additional_map_bytes[VenusReceiverMaxResources];

static void *fixture_calloc(size_t count, size_t bytes) {
    if (fault == 1)
        return NULL;
    void *pointer = calloc(count, bytes);
    assert(pointer);
    allocations++;
    return pointer;
}
static void *fixture_aligned_alloc(size_t alignment, size_t bytes) {
    if (fault == 2)
        return NULL;
    void *pointer = aligned_alloc(alignment, bytes);
    assert(pointer);
    allocations++;
    return pointer;
}
static void fixture_free(void *pointer) {
    if (pointer) {
        assert(allocations > 0);
        allocations--;
        free(pointer);
    }
}
static int fixture_init(void *cookie, int flags, struct virgl_renderer_callbacks *callbacks) {
    assert(!active_renderer);
    assert((flags & VIRGL_RENDERER_RENDER_SERVER) && callbacks->version == 3);
    if (fault == 3)
        return -1;
    active_renderer = 1;
    callback_cookie = cookie;
    callback_table = callbacks;
    return 0;
}
static void fixture_cleanup(void *cookie) {
    assert(active_renderer && cookie == callback_cookie);
    assert(!active_context && !active_resource && !active_map);
    active_renderer = 0;
    callback_cookie = NULL;
    callback_table = NULL;
}
static void fixture_cap_set(uint32_t capset, uint32_t *version, uint32_t *bytes) {
    assert(active_renderer && capset == 4);
    *version = fault == 4 ? 1 : 0;
    *bytes = fault == 5 ? 0 : 160;
}
static void fixture_fill_caps(uint32_t capset, uint32_t version, void *output) {
    assert(capset == 4 && version == 0);
    struct virgl_renderer_capset_venus *caps = output;
    memset(caps, 0, sizeof(*caps));
    caps->supports_blob_id_0 = fault == 6 ? 0 : 1;
}
static int fixture_context_create(uint32_t context, uint32_t flags, uint32_t length,
                                  const char *name) {
    assert(active_renderer && !active_context && context == 1 && flags == 4);
    assert(length == strlen(name));
    if (fault == 7)
        return -1;
    active_context = 1;
    return 0;
}
static void fixture_context_destroy(uint32_t context) {
    assert(context == 1 && active_context);
    /* An outstanding callback runs before this boundary returns. The owner
     * and its callback cookie must remain live through context destruction. */
    callback_table->write_context_fence(callback_cookie, 1, 0, 100);
    callback_table->write_context_fence(callback_cookie, 1, 63, 1);
    active_context = 0;
}
static int fixture_create_blob(const struct virgl_renderer_resource_create_blob_args *blob) {
    if (blob->res_handle != 1) {
        uint32_t slot = blob->res_handle - 2;
        assert(active_context && slot < VenusReceiverMaxResources && !additional_live[slot]);
        assert(blob->ctx_id == 1 && blob->blob_mem == VIRGL_RENDERER_BLOB_MEM_HOST3D);
        if (fault == 14)
            return -1;
        additional_live[slot] = 1;
        additional_bytes[slot] = blob->size;
        active_resource++;
        return 0;
    }
    assert(active_context && !active_resource && blob->res_handle == 1 && blob->ctx_id == 1);
    assert(blob->blob_mem == VIRGL_RENDERER_BLOB_MEM_HOST3D && blob->blob_id == 0 &&
           blob->blob_flags == VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE && blob->size == 4096);
    if (fault == 8)
        return -1;
    active_resource = 1;
    return 0;
}
static int fixture_map(uint32_t resource, void **pointer, uint64_t *bytes) {
    if (resource != 1) {
        uint32_t slot = resource - 2;
        assert(slot < VenusReceiverMaxResources && additional_live[slot] && !additional_maps[slot]);
        if (fault == 15)
            return -1;
        additional_map_bytes[slot] = additional_bytes[slot] + (fault == 19 ? 4096 : 0);
        void *memory = mmap(NULL, (size_t)additional_map_bytes[slot], PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(memory != MAP_FAILED);
        additional_maps[slot] = memory;
        active_map++;
        *pointer = fault == 16 ? NULL : memory;
        *bytes = fault == 17 ? 1 : additional_map_bytes[slot];
        return 0;
    }
    assert(active_resource && resource == 1 && !active_map);
    if (fault == 9)
        return -1;
    *pointer = fault == 10 ? NULL : reply_memory;
    *bytes = fault == 11 ? 1 : sizeof(reply_memory);
    active_map = 1;
    return 0;
}
static int fixture_unmap(uint32_t resource) {
    if (resource != 1) {
        uint32_t slot = resource - 2;
        assert(additional_live[slot] && additional_maps[slot]);
        if (fault == 18)
            return -1;
        assert(munmap(additional_maps[slot], (size_t)additional_map_bytes[slot]) == 0);
        additional_maps[slot] = NULL;
        active_map--;
        return 0;
    }
    assert(resource == 1 && active_map && !active_context);
    active_map = 0;
    return 0;
}
static void fixture_unref(uint32_t resource) {
    if (resource != 1) {
        uint32_t slot = resource - 2;
        assert(additional_live[slot]);
        if (additional_maps[slot]) {
            assert(fault == 18);
            unsigned char residency[2];
            errno = 0;
            assert(mincore(additional_maps[slot], (size_t)additional_bytes[slot], residency) ==
                       -1 &&
                   errno == ENOMEM);
            additional_maps[slot] = NULL;
            active_map--;
        }
        additional_live[slot] = 0;
        additional_bytes[slot] = 0;
        additional_map_bytes[slot] = 0;
        active_resource--;
        return;
    }
    assert(resource == 1 && active_resource && !active_context && !active_map);
    active_resource = 0;
}
static int fixture_submit(void *commands, int context, int words) {
    assert(active_context && context == 1 && words == 2);
    assert((uintptr_t)commands % 64 == 0);
    return fault == 12 ? -1 : 0;
}
static int fixture_fence(uint32_t context, uint32_t flags, uint32_t ring, uint64_t fence) {
    assert(context == 1 && flags == 0 && ring < VenusReceiverTimelineCount && fence != 0);
    if (ring && fence_inline && fault != 13)
        callback_table->write_context_fence(callback_cookie, context, ring, fence);
    return fault == 13 ? -1 : 0;
}
#define calloc fixture_calloc
#define aligned_alloc fixture_aligned_alloc
#define free fixture_free
#define virgl_renderer_init fixture_init
#define virgl_renderer_cleanup fixture_cleanup
static int export_mode;
static int exported_fd = -1;
static int fixture_export(uint32_t id, uint32_t *type, int *fd) {
    assert(id == 3 && additional_live[1]);
    if (export_mode == 1)
        return -1;
    if (export_mode == 2)
        return 0;
    exported_fd = dup(STDERR_FILENO);
    assert(exported_fd >= 0);
    *fd = exported_fd;
    *type = export_mode == 3   ? VIRGL_RENDERER_BLOB_FD_TYPE_OPAQUE
            : export_mode == 4 ? VIRGL_RENDERER_BLOB_FD_TYPE_SHM
                               : VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
    return export_mode == 5 ? -1 : 0;
}
static int fixture_fcntl(int fd, int operation, ...) {
    if (export_mode == 6 && operation == F_GETFD)
        return -1;
    if (export_mode == 7 && operation == F_SETFD)
        return -1;
    return fcntl(fd, operation, FD_CLOEXEC);
}
#define virgl_renderer_resource_export_blob fixture_export
#define fcntl fixture_fcntl
#define virgl_renderer_get_cap_set fixture_cap_set
#define virgl_renderer_fill_caps fixture_fill_caps
#define virgl_renderer_context_create_with_flags fixture_context_create
#define virgl_renderer_context_destroy fixture_context_destroy
#define virgl_renderer_resource_create_blob fixture_create_blob
#define virgl_renderer_resource_map fixture_map
#define virgl_renderer_resource_unmap fixture_unmap
#define virgl_renderer_resource_unref fixture_unref
#define virgl_renderer_submit_cmd fixture_submit
#define virgl_renderer_context_create_fence fixture_fence
#define clock_gettime fixture_clock_gettime
#include "venus_receiver.c"

static void no_resources(void) {
    assert(!allocations && !active_renderer && !active_context && !active_resource && !active_map);
}
static void create(venus_receiver_t **receiver) {
    fault = 0;
    clock_ms = 1000;
    clock_mode = 0;
    assert(venus_receiver_create(receiver, 64, 4096) == RingOk);
    assert(*receiver);
}
static void test_export(void) {
    venus_receiver_t *receiver = NULL;
    int fd = 123;
    assert(venus_receiver_resource_export(NULL, 3, 1, 1, NULL) == RingInvalid);
    assert(venus_receiver_resource_export(NULL, 3, 1, 1, &fd) == RingInvalid && fd == -1);
    create(&receiver);
    assert(venus_receiver_resource_export(receiver, 0, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 66, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_create(receiver, 3, 44, 4096, ResourceShare) == RingOk);
    assert(venus_receiver_resource_create(receiver, 4, 45, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_export(receiver, 2, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 4, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 3, 0, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 3, 1, 0, &fd) == RingInvalid);
    assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingInvalid);
    uint64_t fence;
    assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingOk);
    assert(venus_receiver_resource_export(receiver, 3, 1, fence, &fd) == RingAgain);
    context_fence(receiver, 1, 1, fence);
    const uint32_t Commands[] = {137, 0};
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk);
    assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingAgain);
    context_fence(receiver, 1, 0, fence);
    for (export_mode = 1; export_mode <= 7; export_mode++) {
        exported_fd = -1;
        assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingCorrupt && fd == -1);
        if (exported_fd >= 0) {
            int saved_mode = export_mode;
            export_mode = 0;
            errno = 0;
            assert(fixture_fcntl(exported_fd, F_GETFD) == -1 && errno == EBADF);
            export_mode = saved_mode;
        }
        assert(venus_receiver_poll(receiver) == RingOk);
    }
    export_mode = 0;
    for (int iteration = 0; iteration < 128; iteration++) {
        assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingOk);
        assert(fixture_fcntl(fd, F_GETFD) & FD_CLOEXEC);
        assert(receiver->resource_count == 3 && receiver->resource_bytes == 12288);
        close(fd);
    }
    assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingOk);
    assert(venus_receiver_resource_free(receiver, 3) == RingOk);
    assert(fixture_fcntl(fd, F_GETFD) >= 0);
    close(fd);
    atomic_store_explicit(&receiver->failed, 1, memory_order_release);
    assert(venus_receiver_resource_export(receiver, 2, 1, 1, &fd) == RingInvalid);
    assert(venus_receiver_resource_create(receiver, 3, 44, 4096, ResourceShare) == RingCorrupt);
    receiver->resources[1] =
        (venus_receiver_resource_t){.id = 3, .blob_id = 44, .flags = ResourceShare};
    assert(venus_receiver_resource_export(receiver, 3, 1, 1, &fd) == RingCorrupt);
    memset(&receiver->resources[1], 0, sizeof(receiver->resources[1]));
    venus_receiver_destroy(&receiver);
    no_resources();
}

static void test_resources(void) {
    venus_receiver_t *receiver = NULL;
    uint8_t output[8];
    const uint8_t Input[4] = {1, 2, 3, 4};
    assert(venus_receiver_resource_limits(NULL, 1, 4096) == RingInvalid);
    assert(venus_receiver_resource_create(NULL, 2, 0, 4096, ResourceMap) == RingInvalid);
    assert(venus_receiver_resource_free(NULL, 2) == RingInvalid);
    assert(venus_receiver_resource_read(NULL, 2, 0, output, sizeof(output)) == RingInvalid);
    assert(venus_receiver_resource_write(NULL, 2, 0, Input, sizeof(Input)) == RingInvalid);
    create(&receiver);
    assert(venus_receiver_resource_limits(receiver, 0, 4096) == RingInvalid);
    assert(venus_receiver_resource_limits(receiver, 65, 4096) == RingInvalid);
    assert(venus_receiver_resource_limits(receiver, 1, 4095) == RingInvalid);
    assert(venus_receiver_resource_limits(receiver, 1, UINT64_MAX) == RingInvalid);
    assert(venus_receiver_resource_limits(receiver, 1, 4096) == RingOk);
    assert(venus_receiver_resource_create(receiver, 1, 0, 4096, ResourceMap) == RingInvalid);
    assert(venus_receiver_resource_free(receiver, 0) == RingInvalid);
    assert(venus_receiver_resource_free(receiver, 3) == RingInvalid);
    assert(venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) == RingInvalid);
    assert(venus_receiver_resource_create(receiver, 3, 0, 4096, ResourceMap) == RingLimit);
    assert(venus_receiver_resource_limits(receiver, 1, 8192) == RingInvalid);
    assert(venus_receiver_resource_read(receiver, 66, 0, output, sizeof(output)) == RingInvalid);
    assert(venus_receiver_resource_write(receiver, 3, 0, Input, sizeof(Input)) == RingInvalid);
    assert(venus_receiver_resource_read(receiver, 2, 0, NULL, sizeof(output)) == RingInvalid);
    assert(venus_receiver_resource_write(receiver, 2, 0, NULL, sizeof(Input)) == RingInvalid);
    assert(venus_receiver_resource_read(receiver, 2, UINT64_MAX, output, sizeof(output)) ==
           RingInvalid);
    assert(venus_receiver_resource_write(receiver, 2, 4095, Input, sizeof(Input)) == RingInvalid);
    assert(venus_receiver_resource_write(receiver, 2, 4092, Input, sizeof(Input)) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 4092, output, sizeof(Input)) == RingOk);
    assert(memcmp(output, Input, sizeof(Input)) == 0);
    assert(venus_receiver_resource_free(receiver, 2) == RingOk);
    assert(venus_receiver_resource_free(receiver, 2) == RingInvalid);
    assert(receiver->resource_bytes == 0 && receiver->resource_count == 0);
    assert(venus_receiver_resource_limits(receiver, 2, 4096) == RingOk);
    assert(venus_receiver_resource_create(receiver, 65, 0, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_create(receiver, 64, 0, 4096, ResourceMap) == RingLimit);
    assert(venus_receiver_resource_free(receiver, 65) == RingOk);
    assert(venus_receiver_resource_create(receiver, 2, 1, 4096,
                                          ResourceShare | ResourceCrossDevice) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 0, output, sizeof(output)) == RingInvalid);
    assert(venus_receiver_resource_free(receiver, 2) == RingOk);
    /* Actual nonzero memory blobs use the same bounded mapped copy ownership. */
    assert(venus_receiver_resource_create(receiver, 2, 44, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_write(receiver, 2, 4092, Input, sizeof(Input)) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 4092, output, sizeof(Input)) == RingOk);
    assert(memcmp(output, Input, sizeof(Input)) == 0);
    assert(venus_receiver_resource_read(receiver, 2, 4096, output, 1) == RingInvalid);
    assert(venus_receiver_resource_free(receiver, 2) == RingOk);
    fault = 19; /* SDK device mapping may include inaccessible-to-guest padding. */
    assert(venus_receiver_resource_create(receiver, 2, 45, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 4092, output, 4) == RingOk);
    assert(venus_receiver_resource_write(receiver, 2, 4096, Input, 1) == RingInvalid);
    assert(venus_receiver_resource_free(receiver, 2) == RingOk);
    fault = 0;
    assert(venus_receiver_resource_limits(receiver, 64, VenusReceiverDefaultResourceBytes) ==
           RingOk);
    for (uint32_t id = 2; id <= 65; id++)
        assert(venus_receiver_resource_create(receiver, id, id, 4096, ResourceMap) == RingOk);
    venus_receiver_destroy(&receiver); /* Every unmapped resource released implicitly. */
    no_resources();
    for (int stage = 14; stage <= 18; stage++) {
        create(&receiver);
        fault = stage;
        venus_ring_status_t result =
            venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap);
        if (stage == 14)
            assert(result == RingCorrupt);
        else {
            assert(result == RingOk);
            result = venus_receiver_resource_write(receiver, 2, 0, Input, sizeof(Input));
            if (stage < 18)
                assert(result == RingCorrupt);
            else {
                assert(result == RingOk);
                assert(venus_receiver_resource_free(receiver, 2) == RingCorrupt);
            }
        }
        assert(venus_receiver_resource_limits(receiver, 1, 4096) == RingCorrupt);
        assert(venus_receiver_resource_create(receiver, 3, 0, 4096, ResourceMap) == RingCorrupt);
        assert(venus_receiver_resource_free(receiver, 2) == RingCorrupt);
        assert(venus_receiver_resource_read(receiver, 2, 0, output, sizeof(output)) == RingCorrupt);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
    create(&receiver);
    fault = 19; /* CPU SHM retains its exact declared extent contract. */
    assert(venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 0, output, sizeof(output)) == RingCorrupt);
    venus_receiver_destroy(&receiver);
    no_resources();
    create(&receiver);
    assert(venus_receiver_resource_create(receiver, 2, 0, 4096, ResourceMap) == RingOk);
    assert(venus_receiver_resource_read(receiver, 2, 0, output, sizeof(output)) == RingOk);
    const uint32_t Commands[] = {137, 0};
    uint64_t fence;
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk);
    assert(venus_receiver_resource_limits(receiver, 1, 4096) == RingAgain);
    assert(venus_receiver_resource_create(receiver, 3, 0, 4096, ResourceMap) == RingAgain);
    assert(venus_receiver_resource_free(receiver, 2) == RingAgain);
    assert(venus_receiver_resource_read(receiver, 2, 0, output, sizeof(output)) == RingAgain);
    venus_receiver_destroy(&receiver); /* Mapped resource survives pending context teardown. */
    no_resources();
}

static void *retire_queue(void *context) {
    const uint32_t *timeline = context;
    callback_table->write_context_fence(callback_cookie, 1, *timeline, 1);
    return NULL;
}

static void test_gpu_fences(void) {
    venus_receiver_t *receiver = NULL;
    uint64_t fence = 9;
    assert(venus_receiver_gpu_fence(NULL, 1, &fence) == RingInvalid && fence == 0);
    assert(venus_receiver_gpu_poll(NULL, 1, 1) == RingInvalid);
    create(&receiver);
    assert(venus_receiver_gpu_fence(receiver, 1, NULL) == RingInvalid);
    for (uint32_t timeline = 0; timeline <= 65; timeline++) {
        if (timeline == 0 || timeline >= 64) {
            assert(venus_receiver_gpu_fence(receiver, timeline, &fence) == RingInvalid && !fence);
            assert(venus_receiver_gpu_poll(receiver, timeline, 1) == RingInvalid);
            continue;
        }
        assert(venus_receiver_gpu_poll(receiver, timeline, 0) == RingInvalid);
        assert(venus_receiver_gpu_poll(receiver, timeline, 1) == RingInvalid);
        assert(venus_receiver_gpu_fence(receiver, timeline, &fence) == RingOk && fence == 1);
        assert(venus_receiver_gpu_poll(receiver, timeline, 1) == RingAgain);
        assert(venus_receiver_gpu_fence(receiver, timeline, &fence) == RingAgain && !fence);
    }
    const uint32_t Commands[] = {137, 0};
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk);
    assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingAgain && !fence);
    const uint32_t Timeline = 2;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, retire_queue, (void *)&Timeline) == 0);
    venus_ring_status_t concurrent = venus_receiver_gpu_poll(receiver, 2, 1);
    assert(concurrent == RingAgain || concurrent == RingOk);
    void *joined = (void *)1;
    assert(pthread_join(worker, &joined) == 0 && joined == NULL);
    assert(venus_receiver_gpu_poll(receiver, 2, 1) == RingOk);
    callback_table->write_context_fence(callback_cookie, 1, 1, 1);
    assert(venus_receiver_gpu_poll(receiver, 1, 1) == RingOk);
    assert(venus_receiver_poll(receiver) == RingAgain); /* GPU is not CPU completion. */
    callback_table->write_context_fence(callback_cookie, 1, 0, 1);
    assert(venus_receiver_poll(receiver) == RingOk);
    fence_inline = 1;
    assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingOk && fence == 2);
    assert(venus_receiver_gpu_poll(receiver, 1, 2) == RingOk);
    fence_inline = 0;
    callback_table->write_context_fence(callback_cookie, 1, 1, 1);
    callback_table->write_context_fence(callback_cookie, 1, 1, 2);
    assert(venus_receiver_gpu_poll(receiver, 1, 2) == RingOk); /* No regression. */
    venus_receiver_destroy(&receiver); /* Queue 63 callback while other queues pending. */
    no_resources();
    for (int mode = 0; mode < 4; mode++) {
        create(&receiver);
        if (mode == 0) {
            fault = 13;
            assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingCorrupt && !fence);
        } else if (mode == 1) {
            atomic_store(&receiver->gpu_issued[1], UINT64_MAX);
            atomic_store(&receiver->gpu_retired[1], UINT64_MAX);
            assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingCorrupt && !fence);
        } else if (mode == 2) {
            callback_table->write_context_fence(callback_cookie, 1, 64, 1);
        } else {
            assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingOk);
            callback_table->write_context_fence(callback_cookie, 1, 1, 2);
        }
        assert(venus_receiver_gpu_poll(receiver, 1, 1) == RingCorrupt);
        assert(venus_receiver_gpu_fence(receiver, 2, &fence) == RingCorrupt && !fence);
        assert(venus_receiver_poll(receiver) == RingCorrupt);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
}

static void test_health(void) {
    venus_receiver_t *receiver = NULL;
    const uint32_t Commands[] = {137, 0};
    uint64_t fence;
    _Atomic uint32_t cancel = 0;
    assert(venus_receiver_health(NULL, NULL) == RingInvalid);
    assert(venus_receiver_timeouts(NULL, 1, 1) == RingInvalid);
    create(&receiver);
    assert(venus_receiver_timeouts(receiver, 0, 1) == RingInvalid);
    assert(venus_receiver_timeouts(receiver, 60001, 1) == RingInvalid);
    assert(venus_receiver_timeouts(receiver, 1, 0) == RingInvalid);
    assert(venus_receiver_timeouts(receiver, 1, 60001) == RingInvalid);
    assert(venus_receiver_timeouts(receiver, 5, 10) == RingOk);
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk);
    assert(venus_receiver_timeouts(receiver, 1, 1) == RingAgain);
    clock_ms = 1004;
    assert(venus_receiver_health(receiver, &cancel) == RingOk);
    assert(venus_receiver_poll(receiver) == RingAgain);
    assert(receiver->cpu_deadline_ms == 1005);
    clock_ms = 1005;
    assert(venus_receiver_health(receiver, NULL) == RingTimeout);
    assert(venus_receiver_health(receiver, NULL) == RingCorrupt);
    assert(venus_receiver_timeouts(receiver, 1, 1) == RingCorrupt);
    venus_receiver_destroy(&receiver);
    no_resources();
    create(&receiver);
    assert(venus_receiver_timeouts(receiver, 5, 10) == RingOk);
    assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingOk);
    assert(venus_receiver_timeouts(receiver, 5, 10) == RingAgain);
    clock_ms = 1005;
    assert(venus_receiver_gpu_fence(receiver, 2, &fence) == RingOk);
    clock_ms = 1010;
    callback_table->write_context_fence(callback_cookie, 1, 1, 1);
    assert(venus_receiver_health(receiver, NULL) == RingOk);
    assert(receiver->gpu_deadline_ms[2] == 1015);
    clock_ms = 1015;
    assert(venus_receiver_health(receiver, NULL) == RingTimeout);
    venus_receiver_destroy(&receiver);
    no_resources();
    create(&receiver);
    assert(venus_receiver_timeouts(receiver, 1, 1) == RingOk);
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk);
    clock_ms = 1001;
    callback_table->write_context_fence(callback_cookie, 1, 0, 1);
    assert(venus_receiver_health(receiver, NULL) == RingOk);
    assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingOk);
    clock_ms = 1002;
    callback_table->write_context_fence(callback_cookie, 1, 1, 1);
    assert(venus_receiver_health(receiver, NULL) == RingOk);
    atomic_store_explicit(&cancel, 1, memory_order_release);
    assert(venus_receiver_health(receiver, &cancel) == RingCancelled);
    venus_receiver_destroy(&receiver);
    no_resources();
    for (int mode = 0; mode <= 7; mode++) {
        create(&receiver);
        assert(venus_receiver_health(receiver, NULL) == RingOk);
        clock_mode = mode <= 5 ? mode : 0;
        if (mode == 0)
            clock_ms = 0;
        if (mode == 6)
            clock_ms = 999;
        if (mode == 7)
            clock_ms = UINT64_MAX - 2000;
        if (mode < 7)
            assert(venus_receiver_health(receiver, NULL) == RingCorrupt);
        else
            assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) ==
                   RingCorrupt);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
    for (int gpu = 0; gpu < 2; gpu++) {
        create(&receiver);
        clock_mode = gpu ? 0 : 1;
        clock_ms = gpu ? UINT64_MAX - 2000 : 1000;
        assert(venus_receiver_gpu_fence(receiver, 1, &fence) == RingCorrupt && !fence);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
}

int main(void) {
    venus_receiver_t *receiver = NULL;
    venus_receiver_destroy(NULL);
    venus_receiver_destroy(&receiver);
    assert(venus_receiver_create(NULL, 64, 4096) == RingInvalid);
    assert(venus_receiver_create(&receiver, 65, 4096) == RingInvalid);
    for (fault = 1; fault <= 11; fault++) {
        assert(venus_receiver_create(&receiver, 64, 4096) == RingCorrupt);
        assert(!receiver);
        no_resources();
    }
    create(&receiver);
    assert(venus_receiver_create(&receiver, 64, 4096) == RingInvalid);
    venus_receiver_t *second = NULL;
    assert(venus_receiver_create(&second, 64, 4096) == RingAgain);
    assert(!second);
    uint8_t caps[VenusCapabilityBytes];
    assert(venus_receiver_capabilities(NULL, caps, sizeof(caps)) == RingInvalid);
    assert(venus_receiver_capabilities(receiver, NULL, sizeof(caps)) == RingInvalid);
    assert(venus_receiver_capabilities(receiver, caps, sizeof(caps) - 1) == RingInvalid);
    assert(venus_receiver_capabilities(receiver, caps, sizeof(caps)) == RingOk);
    assert(caps[16] == 1);
    assert(venus_receiver_poll(NULL) == RingInvalid);
    assert(venus_receiver_poll(receiver) == RingOk);
    uint64_t fence = 9;
    const uint32_t Commands[] = {137, 0};
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), NULL) == RingInvalid);
    assert(venus_receiver_submit(NULL, Commands, sizeof(Commands), &fence) == RingInvalid &&
           !fence);
    assert(venus_receiver_submit(receiver, NULL, sizeof(Commands), &fence) == RingInvalid &&
           !fence);
    assert(venus_receiver_submit(receiver, Commands, 7, &fence) == RingInvalid && !fence);
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingOk &&
           fence == 1);
    assert(venus_receiver_poll(receiver) == RingAgain);
    assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingAgain &&
           !fence);
    uint8_t output[8];
    memset(output, 0x5a, sizeof(output));
    assert(venus_receiver_reply(receiver, 0, output, sizeof(output)) == RingAgain);
    for (size_t index = 0; index < sizeof(output); index++)
        assert(output[index] == 0x5a);
    callback_table->write_context_fence(callback_cookie, 1, 0, 1);
    assert(venus_receiver_poll(receiver) == RingOk);
    memcpy(reply_memory, Commands, sizeof(Commands));
    assert(venus_receiver_reply(receiver, 0, output, sizeof(output)) == RingOk);
    assert(memcmp(output, Commands, sizeof(Commands)) == 0);
    assert(venus_receiver_reply(receiver, UINT64_MAX, output, sizeof(output)) == RingInvalid);
    assert(venus_receiver_reply(NULL, 0, output, sizeof(output)) == RingInvalid);
    venus_receiver_destroy(&receiver);
    no_resources();
    for (int stage = 12; stage <= 13; stage++) {
        create(&receiver);
        fault = stage;
        assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) == RingCorrupt &&
               !fence);
        assert(venus_receiver_poll(receiver) == RingCorrupt);
        assert(venus_receiver_reply(receiver, 0, output, sizeof(output)) == RingCorrupt);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
    for (int kind = 0; kind < 5; kind++) {
        create(&receiver);
        if (kind == 0) {
            receiver->submitted = UINT64_MAX;
            atomic_store_explicit(&receiver->retired, UINT64_MAX, memory_order_release);
            assert(venus_receiver_submit(receiver, Commands, sizeof(Commands), &fence) ==
                   RingCorrupt);
        } else if (kind == 1)
            callback_table->write_context_fence(callback_cookie, 2, 0, 1);
        else if (kind == 2)
            callback_table->write_context_fence(callback_cookie, 1, 1, 1);
        else if (kind == 3)
            callback_table->write_context_fence(callback_cookie, 1, 0, 0);
        else
            callback_table->write_fence(callback_cookie, 1);
        assert(venus_receiver_poll(receiver) == RingCorrupt);
        venus_receiver_destroy(&receiver);
        no_resources();
    }
    create(&receiver);
    venus_receiver_destroy(&receiver);
    venus_receiver_destroy(&receiver);
    no_resources();
    test_export();
    test_resources();
    test_gpu_fences();
    test_health();
    return 0;
}
