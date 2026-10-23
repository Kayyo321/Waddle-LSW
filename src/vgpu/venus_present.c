/** @file venus_present.c @brief Bounded Wayland import, feedback and release ownership. */
#include "waddle/venus_present.h"
#include "linux_dmabuf_client.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

/** @brief Presenter-owned accepted image metadata; listeners retain this slot. */
typedef struct present_slot_t {
    struct venus_present_t *owner;             /**< Borrowed owner through completion. */
    struct zwp_linux_buffer_params_v1 *params; /**< Owned during async import. */
    struct wl_buffer *buffer;                  /**< Owned until compositor release. */
    venus_dmabuf_layout_t layout;              /**< Private stable image layout. */
    venus_dmabuf_damage_t damage[64];          /**< Private stable damage rectangles. */
    size_t damage_count;                       /**< Validated 1..64 while accepted. */
    uint64_t frame;                            /**< Nonzero accepted identity, zero if free. */
} present_slot_t;
/** @brief Single-event-thread owner, fixed metadata and three buffer slots. */
struct venus_present_t {
    struct wl_display *display;         /**< Borrowed alive through free. */
    struct zwp_linux_dmabuf_v1 *dmabuf; /**< Borrowed version-four global. */
    struct wl_surface *surface;         /**< Borrowed target surface, caller owns role. */
    struct zwp_linux_dmabuf_feedback_v1 *feedback;     /**< Owned surface feedback. */
    struct wl_callback *frame_callback;                /**< Owned independent pacing callback. */
    present_slot_t slots[3];                           /**< Owned fixed accepted-image ledger. */
    unsigned char tables[2][VenusDmabufMaxTableBytes]; /**< Private double snapshots. */
    unsigned char indices[2][8192];                    /**< Private bounded tranche unions. */
    size_t table_bytes[2], index_bytes[2];             /**< Validated private extents. */
    unsigned active;                                   /**< Completed snapshot index, zero/one. */
    unsigned tranches;                                 /**< Pending completed tranche count. */
    int ready, main_seen, target_seen, formats_seen;   /**< Pending feedback stages. */
    int importing;                                     /**< One pending params proxy. */
    uint64_t last_frame;                               /**< Last accepted identity, never reused. */
    venus_ring_status_t failed;                        /**< Terminal metadata/proxy failure. */
    venus_present_done_t done;                         /**< Borrowed completion callback. */
    void *context;                                     /**< Borrowed caller context. */
};

static void finish_slot(present_slot_t *slot, venus_ring_status_t status) {
    venus_present_t *presenter = slot->owner;
    uint64_t frame = slot->frame;
    if (slot->params) {
        zwp_linux_buffer_params_v1_destroy(slot->params);
        presenter->importing = 0;
    }
    if (slot->buffer)
        wl_buffer_destroy(slot->buffer);
    memset(slot, 0, sizeof(*slot));
    presenter->done(presenter->context, frame, status);
}
static void released_buffer(void *context, struct wl_buffer *buffer) {
    (void)buffer;
    finish_slot(context, RingOk);
}
static const struct wl_buffer_listener BufferEvents = {.release = released_buffer};
static void paced_frame(void *context, struct wl_callback *callback, uint32_t timestamp) {
    (void)timestamp;
    venus_present_t *presenter = context;
    wl_callback_destroy(callback);
    presenter->frame_callback = NULL;
}
static const struct wl_callback_listener FrameEvents = {.done = paced_frame};
static void imported_buffer(void *context, struct zwp_linux_buffer_params_v1 *params,
                            struct wl_buffer *buffer) {
    present_slot_t *slot = context;
    venus_present_t *presenter = slot->owner;
    zwp_linux_buffer_params_v1_destroy(params);
    slot->params = NULL;
    presenter->importing = 0;
    slot->buffer = buffer;
    if (wl_buffer_add_listener(buffer, &BufferEvents, slot) != 0)
        goto fail;
    presenter->frame_callback = wl_surface_frame(presenter->surface);
    if (!presenter->frame_callback)
        goto fail;
    if (wl_callback_add_listener(presenter->frame_callback, &FrameEvents, presenter) != 0) {
        wl_callback_destroy(presenter->frame_callback);
        presenter->frame_callback = NULL;
        goto fail;
    }
    wl_surface_attach(presenter->surface, buffer, 0, 0);
    for (size_t index = 0; index < slot->damage_count; index++) {
        const venus_dmabuf_damage_t *damage = &slot->damage[index];
        wl_surface_damage_buffer(presenter->surface, damage->x, damage->y, damage->width,
                                 damage->height);
    }
    wl_surface_commit(presenter->surface);
    return;
fail:
    presenter->failed = RingCorrupt;
    finish_slot(slot, RingCorrupt);
}
static void rejected_buffer(void *context, struct zwp_linux_buffer_params_v1 *params) {
    (void)params;
    finish_slot(context, RingInvalid);
}
static const struct zwp_linux_buffer_params_v1_listener ParamsEvents = {.created = imported_buffer,
                                                                        .failed = rejected_buffer};

static void feedback_table(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback, int fd,
                           uint32_t bytes) {
    (void)feedback;
    venus_present_t *presenter = context;
    void *mapping = MAP_FAILED;
    struct stat metadata;
    const unsigned char ZeroIndex[2] = {0};
    if (presenter->failed || presenter->tranches || presenter->target_seen ||
        presenter->formats_seen || !bytes || bytes > VenusDmabufMaxTableBytes || bytes % 16 ||
        fstat(fd, &metadata) != 0 || metadata.st_size < (off_t)bytes)
        goto fail;
    mapping = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED)
        goto fail;
    if (venus_dmabuf_feedback_match(mapping, bytes, ZeroIndex, sizeof(ZeroIndex), 0, 0) < 0)
        goto fail;
    unsigned pending = presenter->active ^ 1u;
    memcpy(presenter->tables[pending], mapping, bytes);
    presenter->table_bytes[pending] = bytes;
    presenter->index_bytes[pending] = 0;
    munmap(mapping, bytes);
    close(fd);
    return;
fail:
    if (mapping != MAP_FAILED)
        munmap(mapping, bytes);
    if (fd >= 0)
        close(fd);
    presenter->failed = RingCorrupt;
}
static void feedback_main(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                          struct wl_array *device) {
    (void)feedback;
    venus_present_t *presenter = context;
    if (presenter->main_seen || device->size != sizeof(dev_t) || !device->data)
        presenter->failed = RingCorrupt;
    else
        presenter->main_seen = 1;
}
static void feedback_target(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                            struct wl_array *device) {
    (void)feedback;
    venus_present_t *presenter = context;
    if (presenter->target_seen || device->size != sizeof(dev_t) || !device->data)
        presenter->failed = RingCorrupt;
    else
        presenter->target_seen = 1;
}
static void feedback_formats(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                             struct wl_array *formats) {
    (void)feedback;
    venus_present_t *presenter = context;
    unsigned pending = presenter->active ^ 1u;
    size_t used = presenter->index_bytes[pending];
    if (!presenter->target_seen || presenter->formats_seen || !formats->data ||
        formats->size > sizeof(presenter->indices[pending]) - used ||
        venus_dmabuf_feedback_match(presenter->tables[pending], presenter->table_bytes[pending],
                                    formats->data, formats->size, 0, 0) < 0) {
        presenter->failed = RingCorrupt;
        return;
    }
    memcpy(presenter->indices[pending] + used, formats->data, formats->size);
    presenter->index_bytes[pending] += formats->size;
    presenter->formats_seen = 1;
}
static void feedback_flags(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                           uint32_t flags) {
    (void)context;
    (void)feedback;
    (void)flags; /* Scanout preference cannot authorize unsupported image layouts. */
}
static void feedback_tranche_done(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback) {
    (void)feedback;
    venus_present_t *presenter = context;
    if (!presenter->target_seen || !presenter->formats_seen)
        presenter->failed = RingCorrupt;
    else
        presenter->tranches++;
    presenter->target_seen = presenter->formats_seen = 0;
}
static void feedback_done(void *context, struct zwp_linux_dmabuf_feedback_v1 *feedback) {
    (void)feedback;
    venus_present_t *presenter = context;
    unsigned pending = presenter->active ^ 1u;
    if (presenter->failed || !presenter->main_seen || !presenter->tranches ||
        presenter->target_seen || presenter->formats_seen ||
        venus_dmabuf_feedback_match(presenter->tables[pending], presenter->table_bytes[pending],
                                    presenter->indices[pending], presenter->index_bytes[pending], 0,
                                    0) < 0) {
        presenter->failed = RingCorrupt;
        return;
    }
    presenter->active = pending;
    presenter->ready = 1;
    presenter->main_seen = 0;
    presenter->tranches = 0;
    pending ^= 1u;
    memcpy(presenter->tables[pending], presenter->tables[presenter->active],
           presenter->table_bytes[presenter->active]);
    presenter->table_bytes[pending] = presenter->table_bytes[presenter->active];
    presenter->index_bytes[pending] = 0;
}
static const struct zwp_linux_dmabuf_feedback_v1_listener FeedbackEvents = {
    .done = feedback_done,
    .format_table = feedback_table,
    .main_device = feedback_main,
    .tranche_done = feedback_tranche_done,
    .tranche_target_device = feedback_target,
    .tranche_formats = feedback_formats,
    .tranche_flags = feedback_flags};

venus_ring_status_t venus_present_create(venus_present_t **output, struct wl_display *display,
                                         struct zwp_linux_dmabuf_v1 *dmabuf,
                                         struct wl_surface *surface, venus_present_done_t done,
                                         void *context) {
    if (!output || *output || !display || !dmabuf || !surface || !done ||
        wl_proxy_get_version((struct wl_proxy *)dmabuf) < 4 ||
        wl_proxy_get_version((struct wl_proxy *)surface) < 4)
        return RingInvalid;
    venus_present_t *presenter = calloc(1, sizeof(*presenter));
    if (!presenter)
        return RingCorrupt;
    presenter->display = display;
    presenter->dmabuf = dmabuf;
    presenter->surface = surface;
    presenter->done = done;
    presenter->context = context;
    presenter->feedback = zwp_linux_dmabuf_v1_get_surface_feedback(dmabuf, surface);
    if (!presenter->feedback)
        goto fail;
    if (zwp_linux_dmabuf_feedback_v1_add_listener(presenter->feedback, &FeedbackEvents,
                                                  presenter) != 0)
        goto fail;
    *output = presenter;
    return RingOk;
fail:
    if (presenter->feedback)
        zwp_linux_dmabuf_feedback_v1_destroy(presenter->feedback);
    free(presenter);
    return RingCorrupt;
}

venus_ring_status_t venus_present_submit(venus_present_t *presenter, uint64_t frame,
                                         const venus_dmabuf_layout_t *layout, const int *fds,
                                         size_t plane_count, const venus_dmabuf_damage_t *damage,
                                         size_t damage_count) {
    if (!presenter || !fds || !frame || frame <= presenter->last_frame ||
        venus_dmabuf_layout_validate(layout) != RingOk || plane_count != layout->plane_count ||
        venus_dmabuf_damage_validate(layout->width, layout->height, damage, damage_count) != RingOk)
        return RingInvalid;
    for (size_t index = 0; index < plane_count; index++)
        if (fcntl(fds[index], F_GETFD) < 0)
            return RingInvalid;
    if (presenter->failed)
        return presenter->failed;
    if (!presenter->ready || presenter->importing || presenter->frame_callback)
        return RingAgain;
    unsigned active = presenter->active;
    venus_ring_status_t status = venus_dmabuf_feedback_match(
        presenter->tables[active], presenter->table_bytes[active], presenter->indices[active],
        presenter->index_bytes[active], layout->fourcc, layout->modifier);
    if (status != RingOk)
        return status;
    present_slot_t *slot = NULL;
    for (unsigned index = 0; index < 3; index++)
        if (!presenter->slots[index].frame) {
            slot = &presenter->slots[index];
            break;
        }
    if (!slot)
        return RingAgain;
    slot->params = zwp_linux_dmabuf_v1_create_params(presenter->dmabuf);
    if (!slot->params)
        return RingCorrupt;
    if (zwp_linux_buffer_params_v1_add_listener(slot->params, &ParamsEvents, slot) != 0) {
        zwp_linux_buffer_params_v1_destroy(slot->params);
        slot->params = NULL;
        return RingCorrupt;
    }
    slot->owner = presenter;
    slot->frame = frame;
    slot->layout = *layout;
    slot->damage_count = damage_count;
    memcpy(slot->damage, damage, damage_count * sizeof(*damage));
    for (size_t index = 0; index < plane_count; index++)
        zwp_linux_buffer_params_v1_add(slot->params, fds[index], (uint32_t)index,
                                       layout->planes[index].offset, layout->planes[index].stride,
                                       (uint32_t)(layout->modifier >> 32),
                                       (uint32_t)layout->modifier);
    presenter->importing = 1;
    presenter->last_frame = frame;
    zwp_linux_buffer_params_v1_create(slot->params, (int32_t)layout->width, (int32_t)layout->height,
                                      layout->fourcc, 0);
    return RingOk;
}

venus_ring_status_t venus_present_free(venus_present_t **output) {
    if (!output || !*output)
        return RingOk;
    venus_present_t *presenter = *output;
    int disconnected = wl_display_get_error(presenter->display) != 0;
    if (!disconnected)
        for (unsigned index = 0; index < 3; index++)
            if (presenter->slots[index].frame)
                return RingAgain;
    for (unsigned index = 0; index < 3; index++)
        if (presenter->slots[index].frame)
            finish_slot(&presenter->slots[index], RingClosed);
    if (presenter->frame_callback)
        wl_callback_destroy(presenter->frame_callback);
    zwp_linux_dmabuf_feedback_v1_destroy(presenter->feedback);
    free(presenter);
    *output = NULL;
    return RingOk;
}
