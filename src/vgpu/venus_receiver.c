/** @file venus_receiver.c @brief Scoped public virglrenderer/Venus ownership. */
#include "waddle/venus_receiver.h"
#include "venus_receiver_bounds.h"
#include <stdlib.h>
#include <string.h>
#include <virglrenderer.h>
#include <venus_hw.h>

/** @brief Owned mutable context; external identifiers confined to this adapter. */
struct venus_receiver_t {
    struct virgl_renderer_callbacks callbacks;       /**< Stored until renderer cleanup. */
    struct virgl_renderer_capset_venus capabilities; /**< Pinned trusted ABI snapshot. */
    void *commands;            /**< Owned aligned scratch, freed after renderer cleanup. */
    void *reply;               /**< Borrowed upstream resource map, explicitly unmapped. */
    uint64_t reply_bytes;      /**< Actual validated upstream mapped extent. */
    uint64_t submitted;        /**< Session-thread-only most recently submitted CPU fence. */
    _Atomic uint64_t retired;  /**< Callback release/session acquire fence completion. */
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

static void legacy_fence(void *cookie, uint32_t fence) {
    (void)fence;
    venus_receiver_t *receiver = cookie;
    atomic_store_explicit(&receiver->failed, 1, memory_order_release);
}

static void context_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t fence) {
    venus_receiver_t *receiver = cookie;
    if (context != BootstrapContextId || ring != 0 || fence == 0) {
        atomic_store_explicit(&receiver->failed, 1, memory_order_release);
        return;
    }
    atomic_store_explicit(&receiver->retired, fence, memory_order_release);
}

static void release_owner(venus_receiver_t *receiver) {
    if (receiver->context_created)
        virgl_renderer_context_destroy(BootstrapContextId);
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
    receiver->commands = aligned_alloc(64, command_capacity);
    if (!receiver->commands)
        goto fail;
    receiver->command_capacity = command_capacity;
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

_Static_assert(sizeof(struct virgl_renderer_capset_venus) == VenusCapabilityBytes,
               "Pinned public Venus capability ABI");
