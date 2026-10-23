/** @file venus_session.c @brief Ordered readiness transitions and ring closure. */
#include "waddle/venus_session.h"
#include <string.h>

static int valid_reason(venus_stop_reason_t reason) {
    return reason >= StopDisconnect && reason <= StopProtocol;
}

void venus_session_close(venus_session_t *session, venus_stop_reason_t reason) {
    if (!session || session->state == SessionNone || session->state == SessionClosed)
        return;
    venus_ring_close(&session->region.commands);
    venus_ring_close(&session->region.replies);
    session->reason = valid_reason(reason) ? reason : StopProtocol;
    session->state = SessionClosed;
}

int venus_session_ready(const venus_session_t *session) {
    return session && session->state == SessionReady;
}

venus_ring_status_t venus_session_init(venus_session_t *session, venus_session_role_t role,
                                       void *mapping, size_t length, uint64_t session_id) {
    if (!session)
        return RingInvalid;
    memset(session, 0, sizeof(*session));
    if ((role != SessionHost && role != SessionGuest) ||
        (role == SessionHost ? session_id == 0 : session_id != 0))
        return RingInvalid;
    venus_region_view_t region = {0};
    venus_ring_status_t result = venus_region_attach(&region, mapping, length);
    if (result != RingOk)
        return result;
    if (atomic_load_explicit(&region.commands.header->head, memory_order_acquire) != 0 ||
        atomic_load_explicit(&region.commands.header->tail, memory_order_acquire) != 0 ||
        atomic_load_explicit(&region.replies.header->head, memory_order_acquire) != 0 ||
        atomic_load_explicit(&region.replies.header->tail, memory_order_acquire) != 0)
        return RingInvalid;
    session->region = region;
    session->mapping_bytes = VenusRegionHeaderBytes +
                             2 * (VenusRingHeaderBytes + (uint64_t)region.commands.capacity) +
                             region.resource_bytes;
    session->session_id = session_id;
    session->role = role;
    session->state = SessionInitialized;
    return RingOk;
}

venus_ring_status_t venus_session_send(venus_session_t *session, venus_control_kind_t kind,
                                       venus_stop_reason_t reason, void *frame, size_t length) {
    if (!session || session->state == SessionNone || session->state == SessionClosed || !frame)
        return RingInvalid;
    venus_session_state_t next;
    if (kind == ControlStop) {
        if (!valid_reason(reason) || session->session_id == 0)
            return RingInvalid;
        next = SessionClosed;
    } else {
        if (reason != StopNone)
            return RingInvalid;
        if (kind == ControlOffer && session->role == SessionHost &&
            session->state == SessionInitialized)
            next = SessionOffering;
        else if (kind == ControlAck && session->role == SessionGuest &&
                 session->state == SessionAccepted)
            next = SessionWaiting;
        else if (kind == ControlReady && session->role == SessionHost &&
                 session->state == SessionAcknowledged)
            next = SessionReady;
        else
            return RingInvalid;
    }
    const venus_control_t control = {.kind = kind,
                                     .reason = reason,
                                     .session_id = session->session_id,
                                     .mapping_bytes = session->mapping_bytes,
                                     .capacity = session->region.commands.capacity};
    venus_ring_status_t result = venus_control_encode(&control, frame, length);
    if (result != RingOk)
        return result;
    if (next == SessionClosed)
        venus_session_close(session, reason);
    else
        session->state = next;
    return RingOk;
}

venus_ring_status_t venus_session_receive(venus_session_t *session, const void *frame,
                                          size_t length) {
    if (!session || session->state == SessionNone || !frame)
        return RingInvalid;
    if (session->state == SessionClosed)
        return RingClosed;
    venus_control_t control;
    if (venus_control_decode(&control, frame, length) != RingOk ||
        control.mapping_bytes != session->mapping_bytes ||
        control.capacity != session->region.commands.capacity)
        goto corrupt;
    if (session->state == SessionInitialized && session->role == SessionGuest) {
        if (control.kind != ControlOffer)
            goto corrupt;
        session->session_id = control.session_id;
        session->state = SessionAccepted;
        return RingOk;
    }
    if (control.session_id != session->session_id)
        goto corrupt;
    if (control.kind == ControlStop) {
        venus_session_close(session, (venus_stop_reason_t)control.reason);
        return RingClosed;
    }
    if (session->role == SessionHost && session->state == SessionOffering &&
        control.kind == ControlAck) {
        session->state = SessionAcknowledged;
        return RingOk;
    }
    if (session->role == SessionGuest && session->state == SessionWaiting &&
        control.kind == ControlReady) {
        session->state = SessionReady;
        return RingOk;
    }
corrupt:
    venus_session_close(session, StopProtocol);
    return RingCorrupt;
}
