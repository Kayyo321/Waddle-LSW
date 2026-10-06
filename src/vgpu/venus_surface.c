/** @file venus_surface.c @brief Bounded worker/surface frame identity and FD lifetimes. */
#include "waddle/venus_surface.h"
#include <stdlib.h>
#include <string.h>

/** @brief Private surface owner; all state confined to the display event thread. */
struct venus_surface_t {
    venus_present_t *presenter; /**< Owned native import/feedback proxies. */
    int socket_fd;              /**< Borrowed prepared worker channel. */
    int32_t worker_pid;         /**< Borrowed unreaped sender identity. */
    uint64_t identity;          /**< Immutable controller context binding. */
    uint64_t last_frame;        /**< Latest received identity; never recycled. */
    venus_surface_done_t done;  /**< Borrowed completion callback through free. */
    void *context;              /**< Borrowed callback state through free. */
    venus_ring_status_t failed; /**< Sticky terminal receive/import failure. */
    venus_frame_t pending;      /**< Private unsubmitted frame; zero when absent. */
    int fds[4];                 /**< Owned received descriptors; -1 when absent. */
    venus_frame_t accepted[3];  /**< Metadata retained until compositor retirement. */
    venus_ring_status_t
        release_failed;            /**< Sticky native send loss; never retransmit after loss. */
    int acknowledged;              /**< Nonzero opts into owned native release retries. */
    venus_release_t releases[4];   /**< Completed private records, frame zero means empty. */
    venus_surface_done_t observer; /**< Optional borrowed observer through free. */
    void *observer_context;        /**< Optional borrowed observer context through free. */
};
static venus_ring_status_t flush_releases(venus_surface_t *owner) {
    if (owner->release_failed != RingOk)
        return owner->release_failed;
    for (unsigned index = 0; index < 4; index++) {
        if (!owner->releases[index].frame)
            continue;
        venus_ring_status_t status = venus_release_send(owner->socket_fd, &owner->releases[index]);
        if (status == RingAgain)
            return RingAgain;
        if (status != RingOk) {
            owner->failed = owner->release_failed = status == RingClosed ? RingClosed : RingCorrupt;
            return owner->failed;
        }
        memset(&owner->releases[index], 0, sizeof(owner->releases[index]));
    }
    return RingOk;
}
static void acknowledged_done(void *context, const venus_frame_t *frame,
                              venus_ring_status_t status) {
    venus_surface_t *owner = context;
    if (status != RingOk && status != RingInvalid && status != RingCancelled &&
        status != RingClosed)
        owner->failed = RingCorrupt;
    else {
        unsigned slot;
        for (slot = 0; slot < 4; slot++)
            if (!owner->releases[slot].frame)
                break;
        if (slot == 4)
            owner->failed = RingCorrupt;
        else
            owner->releases[slot] = (venus_release_t){
                .context = frame->context, .frame = frame->frame, .status = status};
    }
    if (owner->observer)
        owner->observer(owner->observer_context, frame, status);
}
static void finish_pending(venus_surface_t *owner, venus_ring_status_t status) {
    venus_frame_t frame = owner->pending;
    memset(&owner->pending, 0, sizeof(owner->pending));
    venus_frame_fds_free(owner->fds);
    if (frame.frame)
        owner->done(owner->context, &frame, status);
}
static void presented(void *context, uint64_t frame, venus_ring_status_t status) {
    venus_surface_t *owner = context;
    for (size_t index = 0; index < 3; index++) {
        if (frame && owner->accepted[index].frame == frame) {
            venus_frame_t completed = owner->accepted[index];
            memset(&owner->accepted[index], 0, sizeof(owner->accepted[index]));
            if (status == RingCorrupt || status == RingClosed)
                owner->failed = status;
            owner->done(owner->context, &completed, status);
            return;
        }
    }
    /* Internal callback identity violation: prohibit receiving further frames. */
    owner->failed = RingCorrupt;
}
venus_ring_status_t venus_surface_create(venus_surface_t **owner, int socket_fd, int32_t worker_pid,
                                         uint64_t identity, struct wl_display *display,
                                         struct zwp_linux_dmabuf_v1 *dmabuf,
                                         struct wl_surface *surface, venus_surface_done_t done,
                                         void *context) {
    if (!owner || *owner || worker_pid <= 0 || !identity || !done ||
        venus_frame_prepare(socket_fd) != RingOk)
        return RingInvalid;
    venus_surface_t *created = calloc(1, sizeof(*created));
    if (!created)
        return RingCorrupt;
    created->socket_fd = socket_fd;
    created->worker_pid = worker_pid;
    created->identity = identity;
    created->done = done;
    created->context = context;
    for (size_t index = 0; index < 4; index++)
        created->fds[index] = -1;
    venus_ring_status_t result =
        venus_present_create(&created->presenter, display, dmabuf, surface, presented, created);
    if (result != RingOk) {
        free(created);
        return result;
    }
    *owner = created;
    return RingOk;
}
venus_ring_status_t
venus_surface_create_acknowledged(venus_surface_t **owner, int socket_fd, int32_t worker_pid,
                                  uint64_t identity, struct wl_display *display,
                                  struct zwp_linux_dmabuf_v1 *dmabuf, struct wl_surface *surface,
                                  venus_surface_done_t observer, void *context) {
    venus_ring_status_t status = venus_surface_create(
        owner, socket_fd, worker_pid, identity, display, dmabuf, surface, acknowledged_done, NULL);
    if (status != RingOk)
        return status;
    (*owner)->acknowledged = 1;
    (*owner)->context = *owner;
    (*owner)->observer = observer;
    (*owner)->observer_context = context;
    return RingOk;
}
venus_ring_status_t venus_surface_poll(venus_surface_t *owner) {
    if (!owner)
        return RingInvalid;
    if (owner->acknowledged) {
        venus_ring_status_t status = flush_releases(owner);
        if (status != RingOk)
            return status;
    }
    if (owner->failed != RingOk)
        return owner->failed;
    size_t slot;
    for (slot = 0; slot < 3; slot++)
        if (!owner->accepted[slot].frame)
            break;
    if (slot == 3)
        return RingAgain;
    if (!owner->pending.frame) {
        venus_ring_status_t result = venus_frame_receive(
            owner->socket_fd, owner->worker_pid, owner->identity, &owner->pending, owner->fds);
        if (result != RingOk) {
            if (result != RingAgain)
                owner->failed = result;
            return result;
        }
        if (owner->pending.frame <= owner->last_frame) {
            venus_frame_fds_free(owner->fds);
            memset(&owner->pending, 0, sizeof(owner->pending));
            owner->failed = RingCorrupt;
            return RingCorrupt;
        }
        owner->last_frame = owner->pending.frame;
    }
    owner->accepted[slot] = owner->pending;
    venus_frame_t *frame = &owner->pending;
    venus_ring_status_t result =
        venus_present_submit(owner->presenter, frame->frame, &frame->layout, owner->fds,
                             frame->layout.plane_count, frame->damage, frame->damage_count);
    if (result == RingOk) {
        memset(frame, 0, sizeof(*frame));
        venus_frame_fds_free(owner->fds);
    } else {
        memset(&owner->accepted[slot], 0, sizeof(owner->accepted[slot]));
        if (result != RingAgain) {
            finish_pending(owner, result);
            if (result != RingInvalid)
                owner->failed = result;
        }
    }
    return result;
}
venus_ring_status_t venus_surface_cancel_pending(venus_surface_t *owner) {
    if (!owner)
        return RingInvalid;
    if (!owner->pending.frame)
        return RingAgain;
    finish_pending(owner, RingInvalid);
    return RingOk;
}
venus_ring_status_t venus_surface_free(venus_surface_t **owner) {
    if (!owner || !*owner)
        return RingOk;
    venus_surface_t *current = *owner;
    venus_ring_status_t flush_status = current->acknowledged ? flush_releases(current) : RingOk;
    if (flush_status == RingAgain)
        return RingAgain;
    venus_ring_status_t result = venus_present_free(&current->presenter);
    if (result != RingOk)
        return result;
    finish_pending(current, RingCancelled);
    if (current->acknowledged && flush_status == RingOk) {
        flush_status = flush_releases(current);
        if (flush_status == RingAgain)
            return RingAgain;
    }
    free(current);
    *owner = NULL;
    return flush_status;
}
