/** @file receiver_owner.c @brief Public renderer boundary acquisition fault fixture. */
#include "waddle/venus_receiver.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <virglrenderer.h>
#include <venus_hw.h>

static int fault;
static int allocations;
static int active_renderer;
static int active_context;
static int active_resource;
static int active_map;
static void *callback_cookie;
static struct virgl_renderer_callbacks *callback_table;
static _Alignas(64) uint8_t reply_memory[4096];

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
    active_context = 0;
}
static int fixture_create_blob(const struct virgl_renderer_resource_create_blob_args *blob) {
    assert(active_context && !active_resource && blob->res_handle == 1 && blob->ctx_id == 1);
    assert(blob->blob_mem == VIRGL_RENDERER_BLOB_MEM_HOST3D && blob->blob_id == 0 &&
           blob->blob_flags == VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE && blob->size == 4096);
    if (fault == 8)
        return -1;
    active_resource = 1;
    return 0;
}
static int fixture_map(uint32_t resource, void **pointer, uint64_t *bytes) {
    assert(active_resource && resource == 1 && !active_map);
    if (fault == 9)
        return -1;
    *pointer = fault == 10 ? NULL : reply_memory;
    *bytes = fault == 11 ? 1 : sizeof(reply_memory);
    active_map = 1;
    return 0;
}
static int fixture_unmap(uint32_t resource) {
    assert(resource == 1 && active_map && !active_context);
    active_map = 0;
    return 0;
}
static void fixture_unref(uint32_t resource) {
    assert(resource == 1 && active_resource && !active_context && !active_map);
    active_resource = 0;
}
static int fixture_submit(void *commands, int context, int words) {
    assert(active_context && context == 1 && words == 2);
    assert((uintptr_t)commands % 64 == 0);
    return fault == 12 ? -1 : 0;
}
static int fixture_fence(uint32_t context, uint32_t flags, uint32_t ring, uint64_t fence) {
    assert(context == 1 && flags == 0 && ring == 0 && fence != 0);
    return fault == 13 ? -1 : 0;
}
#define calloc fixture_calloc
#define aligned_alloc fixture_aligned_alloc
#define free fixture_free
#define virgl_renderer_init fixture_init
#define virgl_renderer_cleanup fixture_cleanup
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
#include "venus_receiver.c"

static void no_resources(void) {
    assert(!allocations && !active_renderer && !active_context && !active_resource && !active_map);
}
static void create(venus_receiver_t **receiver) {
    fault = 0;
    assert(venus_receiver_create(receiver, 64, 4096) == RingOk);
    assert(*receiver);
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
    return 0;
}
