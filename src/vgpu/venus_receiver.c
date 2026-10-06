/** @file venus_receiver.c @brief Scoped public virglrenderer/Venus ownership. */
#include "waddle/venus_receiver.h"
#include "venus_receiver_bounds.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <venus_hw.h>
#include <virglrenderer.h>

/** @brief Receiver-owned additional resource; no private heap allocation.
 * @note ID zero means free. Mapping borrowed from SDK, retained until unmap.
 */
typedef struct venus_receiver_resource_t {
    uint32_t id;      /**< Registry identity, 2..65. */
    uint32_t flags;   /**< Validated local storage policy. */
    uint64_t blob_id; /**< Zero for CPU SHM, otherwise Venus memory identity. */
    uint64_t bytes;   /**< Exact declared storage charged to the quota. */
    void *memory;     /**< Borrowed CPU SHM mapping, never exposed to caller. */
    int mapped;       /**< SDK map ownership stage, even on invalid returned metadata. */
} venus_receiver_resource_t;

/** @brief Owned mutable context; external identifiers confined to this adapter. */
struct venus_receiver_t {
    struct virgl_renderer_callbacks callbacks;       /**< Stored until renderer cleanup. */
    struct virgl_renderer_capset_venus capabilities; /**< Pinned trusted ABI snapshot. */
    venus_receiver_resource_t resources[VenusReceiverMaxResources]; /**< Owned fixed ledger. */
    uint64_t resource_limit;       /**< Configured additional declared storage bytes. */
    uint64_t resource_bytes;       /**< Sum of live ledger sizes. */
    uint32_t resource_count_limit; /**< Configured additional entry limit. */
    uint32_t resource_count;       /**< Number of live ledger IDs. */
    void *commands;                /**< Owned aligned scratch, freed after renderer cleanup. */
    void *reply;                   /**< Borrowed upstream resource map, explicitly unmapped. */
    uint64_t reply_bytes;          /**< Actual validated upstream mapped extent. */
    uint64_t cpu_deadline_ms;      /**< Absolute pending CPU deadline, session-thread-only. */
    uint64_t gpu_deadline_ms[VenusReceiverTimelineCount]; /**< Per-queue absolute budgets. */
    uint64_t last_clock_ms;                               /**< Last validated monotonic sample. */
    uint32_t cpu_timeout_ms;                              /**< Trusted host CPU policy. */
    uint32_t gpu_timeout_ms;                              /**< Trusted host per-queue GPU policy. */
    uint64_t submitted;       /**< Session-thread-only most recently submitted CPU fence. */
    _Atomic uint64_t retired; /**< Callback release/session acquire fence completion. */
    _Atomic uint64_t gpu_issued[VenusReceiverTimelineCount];  /**< Published queue fence IDs. */
    _Atomic uint64_t gpu_retired[VenusReceiverTimelineCount]; /**< Callback retired maximum. */
    _Atomic int failed;        /**< Public failure or unexpected callback identity. */
    uint32_t command_capacity; /**< Immutable local scratch allocation bytes. */
    int initialized;           /**< Renderer cleanup required. */
    int context_created;       /**< Context destruction required; callbacks may be active. */
    int resource_created;      /**< Resource unref required. */
    int mapped;                /**< Resource unmap required even on returned extent mismatch. */
};
static atomic_flag receiver_claim = ATOMIC_FLAG_INIT;
/** @brief Pinned Venus capability set ID and owned bootstrap resource identities. */
#define VenusCapsetId 4u
/** @brief Single bootstrap context ID; never provided by an untrusted peer. */
#define BootstrapContextId 1u
/** @brief Single reply blob ID; bound to BootstrapContextId. */
#define BootstrapReplyId 1u

static int sample_clock(venus_receiver_t *receiver, uint64_t *milliseconds) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000 || (uint64_t)now.tv_sec > (UINT64_MAX - 999) / 1000)
        return 0;
    *milliseconds = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    if (!*milliseconds || *milliseconds < receiver->last_clock_ms)
        return 0;
    receiver->last_clock_ms = *milliseconds;
    return 1;
}

static void legacy_fence(void *cookie, uint32_t fence) {
    (void)fence;
    venus_receiver_t *receiver = cookie;
    atomic_store_explicit(&receiver->failed, 1, memory_order_release);
}

static void context_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t fence) {
    venus_receiver_t *receiver = cookie;
    if (context != BootstrapContextId || ring >= VenusReceiverTimelineCount || fence == 0 ||
        (ring && fence > atomic_load_explicit(&receiver->gpu_issued[ring], memory_order_acquire))) {
        atomic_store_explicit(&receiver->failed, 1, memory_order_release);
        return;
    }
    if (!ring) {
        atomic_store_explicit(&receiver->retired, fence, memory_order_release);
        return;
    }
    uint64_t retired = atomic_load_explicit(&receiver->gpu_retired[ring], memory_order_relaxed);
    while (retired < fence &&
           !atomic_compare_exchange_weak_explicit(&receiver->gpu_retired[ring], &retired, fence,
                                                  memory_order_release, memory_order_relaxed)) {
    }
}

static void release_resource(venus_receiver_resource_t *resource) {
    if (!resource->id)
        return;
    if (resource->mapped && virgl_renderer_resource_unmap(resource->id) != 0 && resource->memory)
        (void)munmap(resource->memory, (size_t)resource->bytes);
    virgl_renderer_resource_unref(resource->id);
    memset(resource, 0, sizeof(*resource));
}

static void release_owner(venus_receiver_t *receiver) {
    if (receiver->context_created)
        virgl_renderer_context_destroy(BootstrapContextId);
    for (uint32_t index = 0; index < VenusReceiverMaxResources; index++)
        release_resource(&receiver->resources[index]);
    if (receiver->mapped)
        (void)virgl_renderer_resource_unmap(BootstrapReplyId);
    receiver->reply = NULL;
    if (receiver->resource_created)
        virgl_renderer_resource_unref(BootstrapReplyId);
    if (receiver->initialized)
        virgl_renderer_cleanup(receiver);
    free(receiver->commands);
    receiver->commands = NULL;
    free(receiver);
}

void venus_receiver_destroy(venus_receiver_t **output) {
    if (!output || !*output)
        return;
    venus_receiver_t *receiver = *output;
    *output = NULL;
    release_owner(receiver);
    atomic_flag_clear_explicit(&receiver_claim, memory_order_release);
}

venus_ring_status_t venus_receiver_create(venus_receiver_t **output, uint32_t command_capacity,
                                          uint64_t reply_bytes) {
    if (!output || *output || !venus_receiver_limits(command_capacity, reply_bytes))
        return RingInvalid;
    if (atomic_flag_test_and_set_explicit(&receiver_claim, memory_order_acquire))
        return RingAgain;
    venus_receiver_t *receiver = calloc(1, sizeof(*receiver));
    if (!receiver)
        goto fail;
    atomic_init(&receiver->retired, 0);
    atomic_init(&receiver->failed, 0);
    for (uint32_t timeline = 0; timeline < VenusReceiverTimelineCount; timeline++) {
        atomic_init(&receiver->gpu_issued[timeline], 0);
        atomic_init(&receiver->gpu_retired[timeline], 0);
    }
    receiver->commands = aligned_alloc(64, command_capacity);
    if (!receiver->commands)
        goto fail;
    receiver->command_capacity = command_capacity;
    receiver->cpu_timeout_ms = VenusReceiverDefaultTimeoutMs;
    receiver->gpu_timeout_ms = VenusReceiverDefaultTimeoutMs;
    receiver->resource_limit = VenusReceiverDefaultResourceBytes;
    receiver->resource_count_limit = VenusReceiverMaxResources;
    receiver->callbacks.version = 3;
    receiver->callbacks.write_fence = legacy_fence;
    receiver->callbacks.write_context_fence = context_fence;
    const int Flags = VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL |
                      VIRGL_RENDERER_RENDER_SERVER | VIRGL_RENDERER_USE_EXTERNAL_BLOB |
                      VIRGL_RENDERER_THREAD_SYNC | VIRGL_RENDERER_ASYNC_FENCE_CB;
    if (virgl_renderer_init(receiver, Flags, &receiver->callbacks) != 0)
        goto fail;
    receiver->initialized = 1;
    uint32_t version = 0, bytes = 0;
    virgl_renderer_get_cap_set(VenusCapsetId, &version, &bytes);
    if (version != 0 || bytes != sizeof(receiver->capabilities))
        goto fail;
    virgl_renderer_fill_caps(VenusCapsetId, version, &receiver->capabilities);
    if (!receiver->capabilities.supports_blob_id_0)
        goto fail;
    const char Name[] = "waddle-vgpu";
    if (virgl_renderer_context_create_with_flags(BootstrapContextId, VenusCapsetId,
                                                 sizeof(Name) - 1, Name) != 0)
        goto fail;
    receiver->context_created = 1;
    const struct virgl_renderer_resource_create_blob_args blob = {
        .res_handle = BootstrapReplyId,
        .ctx_id = BootstrapContextId,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
        .blob_flags = VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE,
        .blob_id = 0,
        .size = reply_bytes};
    if (virgl_renderer_resource_create_blob(&blob) != 0)
        goto fail;
    receiver->resource_created = 1;
    if (virgl_renderer_resource_map(BootstrapReplyId, &receiver->reply, &receiver->reply_bytes) !=
        0)
        goto fail;
    receiver->mapped = 1;
    if (!receiver->reply || receiver->reply_bytes != reply_bytes)
        goto fail;
    memset(receiver->reply, 0, (size_t)reply_bytes);
    *output = receiver;
    return RingOk;
fail:
    if (receiver)
        release_owner(receiver);
    atomic_flag_clear_explicit(&receiver_claim, memory_order_release);
    return RingCorrupt;
}

venus_ring_status_t venus_receiver_capabilities(const venus_receiver_t *receiver, void *output,
                                                size_t length) {
    if (!receiver || !output || length != VenusCapabilityBytes)
        return RingInvalid;
    memcpy(output, &receiver->capabilities, VenusCapabilityBytes);
    return RingOk;
}

venus_ring_status_t venus_receiver_poll(const venus_receiver_t *receiver) {
    if (!receiver)
        return RingInvalid;
    if (atomic_load_explicit(&receiver->failed, memory_order_acquire))
        return RingCorrupt;
    return atomic_load_explicit(&receiver->retired, memory_order_acquire) >= receiver->submitted
               ? RingOk
               : RingAgain;
}

venus_ring_status_t venus_receiver_submit(venus_receiver_t *receiver, const void *commands,
                                          size_t length, uint64_t *fence) {
    if (!fence)
        return RingInvalid;
    *fence = 0;
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    if (venus_receiver_command_copy(receiver->commands, receiver->command_capacity, commands,
                                    length) != 0)
        return RingInvalid;
    uint64_t now;
    if (!sample_clock(receiver, &now) || now > UINT64_MAX - receiver->cpu_timeout_ms)
        goto fail;
    receiver->cpu_deadline_ms = now + receiver->cpu_timeout_ms;
    if (receiver->submitted == UINT64_MAX ||
        virgl_renderer_submit_cmd(receiver->commands, BootstrapContextId, (int)(length / 4)) != 0)
        goto fail;
    receiver->submitted++;
    if (virgl_renderer_context_create_fence(BootstrapContextId, 0, 0, receiver->submitted) != 0)
        goto fail;
    *fence = receiver->submitted;
    return RingOk;
fail:
    atomic_store_explicit(&receiver->failed, 1, memory_order_release);
    return RingCorrupt;
}

venus_ring_status_t venus_receiver_reply(const venus_receiver_t *receiver, uint64_t offset,
                                         void *output, size_t length) {
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    return (venus_ring_status_t)venus_receiver_reply_copy(output, receiver->reply,
                                                          receiver->reply_bytes, offset, length);
}

static venus_ring_status_t poison_receiver(venus_receiver_t *receiver) {
    atomic_store_explicit(&receiver->failed, 1, memory_order_release);
    return RingCorrupt;
}

venus_ring_status_t venus_receiver_resource_limits(venus_receiver_t *receiver, uint32_t count,
                                                   uint64_t bytes) {
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    if (!count || count > VenusReceiverMaxResources || bytes < 4096 || bytes > 1073741824 ||
        receiver->resource_count)
        return RingInvalid;
    receiver->resource_count_limit = count;
    receiver->resource_limit = bytes;
    return RingOk;
}

venus_ring_status_t venus_receiver_resource_create(venus_receiver_t *receiver, uint32_t id,
                                                   uint64_t blob_id, uint64_t bytes,
                                                   uint32_t flags) {
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    if (!venus_receiver_resource_request(id, blob_id, bytes, flags))
        return RingInvalid;
    venus_receiver_resource_t *resource =
        &receiver->resources[venus_receiver_resource_slot(id) - 1];
    if (resource->id)
        return RingInvalid;
    if (receiver->resource_count == receiver->resource_count_limit ||
        bytes > receiver->resource_limit - receiver->resource_bytes)
        return RingLimit;
    uint32_t native_flags = 0;
    if (flags & ResourceMap)
        native_flags |= VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE;
    if (flags & ResourceShare)
        native_flags |= VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE;
    if (flags & ResourceCrossDevice)
        native_flags |= VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE;
    const struct virgl_renderer_resource_create_blob_args blob = {
        .res_handle = id,
        .ctx_id = BootstrapContextId,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
        .blob_flags = native_flags,
        .blob_id = blob_id,
        .size = bytes};
    if (virgl_renderer_resource_create_blob(&blob) != 0)
        return poison_receiver(receiver);
    *resource =
        (venus_receiver_resource_t){.id = id, .flags = flags, .blob_id = blob_id, .bytes = bytes};
    receiver->resource_bytes += bytes;
    receiver->resource_count++;
    return RingOk;
}

static venus_receiver_resource_t *find_resource(venus_receiver_t *receiver, uint32_t id) {
    uint32_t slot = venus_receiver_resource_slot(id);
    return slot && receiver->resources[slot - 1].id ? &receiver->resources[slot - 1] : NULL;
}

venus_ring_status_t venus_receiver_resource_free(venus_receiver_t *receiver, uint32_t id) {
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    venus_receiver_resource_t *resource = find_resource(receiver, id);
    if (!resource)
        return RingInvalid;
    if (resource->mapped) {
        if (virgl_renderer_resource_unmap(id) != 0)
            return poison_receiver(receiver);
        resource->mapped = 0;
        resource->memory = NULL;
    }
    receiver->resource_bytes -= resource->bytes;
    receiver->resource_count--;
    release_resource(resource);
    return RingOk;
}

static venus_ring_status_t copy_resource(venus_receiver_t *receiver, uint32_t id, uint64_t offset,
                                         const void *input, void *output, size_t length,
                                         int writing) {
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    venus_receiver_resource_t *resource = find_resource(receiver, id);
    if (!resource || !(resource->flags & ResourceMap) || (writing ? !input : !output) ||
        !venus_receiver_resource_range(resource->bytes, offset, length))
        return RingInvalid;
    if (!resource->mapped) {
        uint64_t bytes = 0;
        if (virgl_renderer_resource_map(id, &resource->memory, &bytes) != 0)
            return poison_receiver(receiver);
        resource->mapped = 1;
        if (!resource->memory || bytes < resource->bytes ||
            (!resource->blob_id && bytes != resource->bytes))
            return poison_receiver(receiver);
    }
    return writing
               ? (venus_ring_status_t)venus_receiver_memory_write(resource->memory, resource->bytes,
                                                                  offset, input, length)
               : (venus_ring_status_t)venus_receiver_reply_copy(output, resource->memory,
                                                                resource->bytes, offset, length);
}

venus_ring_status_t venus_receiver_resource_read(venus_receiver_t *receiver, uint32_t id,
                                                 uint64_t offset, void *output, size_t length) {
    return copy_resource(receiver, id, offset, NULL, output, length, 0);
}

venus_ring_status_t venus_receiver_resource_write(venus_receiver_t *receiver, uint32_t id,
                                                  uint64_t offset, const void *input,
                                                  size_t length) {
    return copy_resource(receiver, id, offset, input, NULL, length, 1);
}

venus_ring_status_t venus_receiver_gpu_poll(const venus_receiver_t *receiver, uint32_t timeline,
                                            uint64_t fence) {
    if (!receiver || !venus_receiver_gpu_timeline(timeline) || !fence)
        return RingInvalid;
    if (atomic_load_explicit(&receiver->failed, memory_order_acquire))
        return RingCorrupt;
    if (fence > atomic_load_explicit(&receiver->gpu_issued[timeline], memory_order_acquire))
        return RingInvalid;
    return atomic_load_explicit(&receiver->gpu_retired[timeline], memory_order_acquire) >= fence
               ? RingOk
               : RingAgain;
}

venus_ring_status_t venus_receiver_gpu_fence(venus_receiver_t *receiver, uint32_t timeline,
                                             uint64_t *fence) {
    if (!fence)
        return RingInvalid;
    *fence = 0;
    if (!venus_receiver_gpu_timeline(timeline))
        return RingInvalid;
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    uint64_t issued = atomic_load_explicit(&receiver->gpu_issued[timeline], memory_order_acquire);
    if (atomic_load_explicit(&receiver->gpu_retired[timeline], memory_order_acquire) < issued)
        return RingAgain;
    uint64_t now;
    if (issued == UINT64_MAX || !sample_clock(receiver, &now) ||
        now > UINT64_MAX - receiver->gpu_timeout_ms)
        return poison_receiver(receiver);
    receiver->gpu_deadline_ms[timeline] = now + receiver->gpu_timeout_ms;
    atomic_store_explicit(&receiver->gpu_issued[timeline], issued + 1, memory_order_release);
    if (virgl_renderer_context_create_fence(BootstrapContextId, 0, timeline, issued + 1) != 0)
        return poison_receiver(receiver);
    *fence = issued + 1;
    return RingOk;
}

venus_ring_status_t venus_receiver_timeouts(venus_receiver_t *receiver, uint32_t cpu_ms,
                                            uint32_t gpu_ms) {
    if (!cpu_ms || cpu_ms > 60000 || !gpu_ms || gpu_ms > 60000)
        return RingInvalid;
    venus_ring_status_t result = venus_receiver_poll(receiver);
    if (result != RingOk)
        return result;
    for (uint32_t timeline = 1; timeline < VenusReceiverTimelineCount; timeline++)
        if (atomic_load_explicit(&receiver->gpu_retired[timeline], memory_order_acquire) <
            atomic_load_explicit(&receiver->gpu_issued[timeline], memory_order_acquire))
            return RingAgain;
    receiver->cpu_timeout_ms = cpu_ms;
    receiver->gpu_timeout_ms = gpu_ms;
    return RingOk;
}

venus_ring_status_t venus_receiver_health(venus_receiver_t *receiver,
                                          const _Atomic uint32_t *cancel) {
    if (!receiver)
        return RingInvalid;
    if (atomic_load_explicit(&receiver->failed, memory_order_acquire))
        return RingCorrupt;
    if (cancel && atomic_load_explicit(cancel, memory_order_acquire)) {
        (void)poison_receiver(receiver);
        return RingCancelled;
    }
    uint64_t now;
    if (!sample_clock(receiver, &now))
        return poison_receiver(receiver);
    int expired =
        atomic_load_explicit(&receiver->retired, memory_order_acquire) < receiver->submitted &&
        now >= receiver->cpu_deadline_ms;
    for (uint32_t timeline = 1; timeline < VenusReceiverTimelineCount; timeline++)
        expired |=
            atomic_load_explicit(&receiver->gpu_retired[timeline], memory_order_acquire) <
                atomic_load_explicit(&receiver->gpu_issued[timeline], memory_order_acquire) &&
            now >= receiver->gpu_deadline_ms[timeline];
    if (expired) {
        (void)poison_receiver(receiver);
        return RingTimeout;
    }
    return RingOk;
}

_Static_assert(sizeof(struct virgl_renderer_capset_venus) == VenusCapabilityBytes,
               "Pinned public Venus capability ABI");

venus_ring_status_t venus_receiver_resource_export(venus_receiver_t *receiver, uint32_t id,
                                                   uint32_t timeline, uint64_t fence, int *output) {
    if (!output)
        return RingInvalid;
    *output = -1;
    uint32_t slot = venus_receiver_resource_slot(id);
    if (!receiver || !slot)
        return RingInvalid;
    const venus_receiver_resource_t *resource = &receiver->resources[slot - 1];
    if (!resource->id || !resource->blob_id || !(resource->flags & ResourceShare))
        return RingInvalid;
    venus_ring_status_t status = venus_receiver_poll(receiver);
    if (status != RingOk)
        return status;
    status = venus_receiver_gpu_poll(receiver, timeline, fence);
    if (status != RingOk)
        return status;
    uint32_t fd_type = 0;
    int fd = -1;
    if (virgl_renderer_resource_export_blob(id, &fd_type, &fd) != 0 || fd < 0 ||
        fd_type != VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF)
        goto fail;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        goto fail;
    *output = fd;
    return RingOk;
fail:
    if (fd >= 0)
        close(fd);
    return RingCorrupt;
}
