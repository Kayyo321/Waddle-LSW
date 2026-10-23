/** @file venus_export.c @brief Fence-bound worker export and exact-once completion ownership. */
#include "waddle/venus_export.h"
#include <string.h>
#include <unistd.h>
venus_ring_status_t venus_export_init(venus_export_t *owner, venus_receiver_t *receiver,
                                      int socket_fd, int32_t controller_pid, uint64_t context) {
    if (!owner || owner->receiver || !receiver || controller_pid <= 0 || !context ||
        venus_frame_prepare(socket_fd) != RingOk)
        return RingInvalid;
    *owner = (venus_export_t){.receiver = receiver,
                              .socket_fd = socket_fd,
                              .controller_pid = controller_pid,
                              .context = context};
    return RingOk;
}
void venus_export_free(venus_export_t *owner) {
    if (owner)
        memset(owner, 0, sizeof(*owner));
}
int venus_export_resource_busy(const venus_export_t *owner, uint32_t resource_id) {
    if (!owner || !owner->receiver || resource_id < 2 || resource_id > 65)
        return 0;
    for (unsigned slot = 0; slot < VenusExportSlots; slot++)
        if (owner->leases[slot].release.frame)
            for (unsigned plane = 0; plane < 4; plane++)
                if (owner->leases[slot].resource_ids[plane] == resource_id)
                    return 1;
    return 0;
}
venus_ring_status_t venus_export_pump(venus_export_t *owner) {
    if (!owner || !owner->receiver)
        return RingInvalid;
    if (owner->terminal != RingOk)
        return owner->terminal;
    for (unsigned packet = 0; packet < VenusExportSlots; packet++) {
        venus_release_t release;
        venus_ring_status_t status = venus_release_receive(owner->socket_fd, owner->controller_pid,
                                                           owner->context, &release);
        if (status == RingAgain)
            return RingOk;
        if (status != RingOk)
            return owner->terminal = status;
        venus_export_lease_t *lease = NULL;
        for (unsigned slot = 0; slot < VenusExportSlots; slot++)
            if (owner->leases[slot].release.frame == release.frame)
                lease = &owner->leases[slot];
        if (!lease || lease->completed)
            return owner->terminal = RingCorrupt;
        lease->release = release;
        lease->completed = 1;
    }
    return RingOk;
}
venus_ring_status_t venus_export_submit(venus_export_t *owner, const void *bytes, size_t length,
                                        uint32_t timeline, uint64_t fence) {
    if (!owner || !owner->receiver || !timeline || timeline >= 64 || !fence)
        return RingInvalid;
    if (owner->terminal != RingOk)
        return owner->terminal;
    venus_frame_t frame;
    if (venus_frame_decode(&frame, bytes, length) != RingOk || frame.context != owner->context ||
        frame.frame <= owner->last_frame)
        return RingInvalid;
    venus_export_lease_t *lease = NULL;
    for (unsigned slot = 0; slot < VenusExportSlots; slot++)
        if (!owner->leases[slot].release.frame && !lease)
            lease = &owner->leases[slot];
    if (!lease)
        return RingAgain;
    for (unsigned plane = 0; plane < frame.layout.plane_count; plane++)
        if (venus_export_resource_busy(owner, frame.resource_ids[plane]))
            return RingAgain;
    int fds[4] = {-1, -1, -1, -1};
    venus_ring_status_t status = RingOk;
    for (unsigned plane = 0; plane < frame.layout.plane_count; plane++) {
        status = venus_receiver_resource_export(owner->receiver, frame.resource_ids[plane],
                                                timeline, fence, &fds[plane]);
        if (status != RingOk)
            goto cleanup;
        off_t extent = lseek(fds[plane], 0, SEEK_END);
        if (extent < 0 || (uint64_t)extent != frame.layout.planes[plane].extent) {
            status = RingInvalid;
            goto cleanup;
        }
    }
    status = venus_frame_send(owner->socket_fd, &frame, fds, frame.layout.plane_count);
    if (status == RingOk) {
        lease->release = (venus_release_t){.context = frame.context, .frame = frame.frame};
        memcpy(lease->resource_ids, frame.resource_ids, sizeof(lease->resource_ids));
        owner->last_frame = frame.frame;
    }
cleanup:
    venus_frame_fds_free(fds);
    if (status == RingClosed || status == RingCorrupt || status == RingCancelled ||
        status == RingTimeout)
        owner->terminal = status;
    return status;
}
venus_ring_status_t venus_export_take(venus_export_t *owner, uint64_t frame,
                                      venus_release_t *release) {
    if (!release)
        return RingInvalid;
    memset(release, 0, sizeof(*release));
    if (!frame)
        return RingInvalid;
    venus_ring_status_t status = venus_export_pump(owner);
    if (status != RingOk)
        return status;
    for (unsigned slot = 0; slot < VenusExportSlots; slot++) {
        venus_export_lease_t *lease = &owner->leases[slot];
        if (lease->release.frame == frame) {
            if (!lease->completed)
                return RingAgain;
            *release = lease->release;
            memset(lease, 0, sizeof(*lease));
            return RingOk;
        }
    }
    return RingInvalid;
}
