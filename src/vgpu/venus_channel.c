/** @file venus_channel.c @brief Exact lifecycle framing and bounded ring waits. */
#include "venus_stream.h"
#include <string.h>

static venus_ring_status_t check_deadline(venus_channel_t *channel) {
    if (channel->cancel && atomic_load_explicit(channel->cancel, memory_order_acquire)) {
        venus_session_close(channel->session, StopCancel);
        return RingCancelled;
    }
    uint64_t now = venus_stream_time_ms();
    if (!now) {
        venus_session_close(channel->session, StopDisconnect);
        return RingClosed;
    }
    if (now >= channel->deadline_ms) {
        venus_session_close(channel->session, StopDeadline);
        return RingTimeout;
    }
    return RingOk;
}

venus_ring_status_t venus_channel_init(venus_channel_t *channel, venus_session_t *session,
                                       intptr_t stream, const _Atomic uint32_t *cancel) {
    if (!channel)
        return RingInvalid;
    memset(channel, 0, sizeof(*channel));
    if (!session || session->state != SessionInitialized)
        return RingInvalid;
    venus_channel_t pending = {.session = session, .stream = stream, .cancel = cancel};
    if (venus_stream_open(&pending) != RingOk)
        return RingInvalid;
    pending.initialized = 1;
    *channel = pending;
    return RingOk;
}

venus_ring_status_t venus_channel_deadline(venus_channel_t *channel, uint32_t timeout_ms) {
    if (!channel || !channel->initialized || !timeout_ms || timeout_ms > 60000 || channel->received)
        return RingInvalid;
    uint64_t now = venus_stream_time_ms();
    if (!now || now > UINT64_MAX - timeout_ms) {
        venus_session_close(channel->session, StopDisconnect);
        return RingClosed;
    }
    channel->deadline_ms = now + timeout_ms;
    return RingOk;
}

uint64_t venus_channel_time_ms(void) {
    return venus_stream_time_ms();
}

venus_ring_status_t venus_channel_deadline_until(venus_channel_t *channel, uint64_t deadline_ms) {
    if (!channel || !channel->initialized || !deadline_ms || channel->received)
        return RingInvalid;
    if (channel->session->state == SessionClosed)
        return RingClosed;
    if (channel->cancel && atomic_load_explicit(channel->cancel, memory_order_acquire)) {
        venus_session_close(channel->session, StopCancel);
        return RingCancelled;
    }
    uint64_t now = venus_stream_time_ms();
    if (!now) {
        venus_session_close(channel->session, StopDisconnect);
        return RingClosed;
    }
    if (deadline_ms <= now) {
        venus_session_close(channel->session, StopDeadline);
        return RingTimeout;
    }
    if (deadline_ms - now > 60000)
        return RingInvalid;
    channel->deadline_ms = deadline_ms;
    return RingOk;
}

static venus_ring_status_t io_failure(venus_channel_t *channel, venus_ring_status_t status,
                                      size_t received) {
    if (status == RingClosed && received) {
        venus_session_close(channel->session, StopProtocol);
        return RingCorrupt;
    }
    venus_session_close(channel->session, StopDisconnect);
    return status;
}

static venus_ring_status_t send_frame(venus_channel_t *channel, void *frame) {
    size_t sent = 0;
    while (sent < VenusControlBytes) {
        venus_ring_status_t result = check_deadline(channel);
        if (result != RingOk)
            return result;
        size_t count = 0;
        result =
            venus_stream_io(channel, (uint8_t *)frame + sent, VenusControlBytes - sent, 1, &count);
        if (result == RingAgain)
            continue;
        if (result != RingOk)
            return io_failure(channel, result, 0);
        sent += count;
    }
    return check_deadline(channel);
}

static venus_ring_status_t receive_tick(venus_channel_t *channel) {
    venus_ring_status_t result = check_deadline(channel);
    if (result != RingOk)
        return result;
    size_t count = 0;
    result = venus_stream_io(channel, channel->incoming + channel->received,
                             VenusControlBytes - channel->received, 0, &count);
    if (result == RingAgain)
        return RingAgain;
    if (result != RingOk)
        return io_failure(channel, result, channel->received);
    channel->received += count;
    if (channel->received < VenusControlBytes)
        return RingAgain;
    channel->received = 0;
    result = check_deadline(channel);
    if (result != RingOk)
        return result;
    return venus_session_receive(channel->session, channel->incoming, VenusControlBytes);
}

static venus_ring_status_t receive_frame(venus_channel_t *channel) {
    venus_ring_status_t result;
    do {
        result = receive_tick(channel);
    } while (result == RingAgain);
    return result;
}

static venus_ring_status_t send_kind(venus_channel_t *channel, venus_control_kind_t kind,
                                     venus_stop_reason_t reason) {
    uint8_t frame[VenusControlBytes];
    venus_ring_status_t result =
        venus_session_send(channel->session, kind, reason, frame, sizeof(frame));
    return result == RingOk ? send_frame(channel, frame) : result;
}

venus_ring_status_t venus_channel_handshake(venus_channel_t *channel) {
    if (!channel || !channel->initialized || !channel->deadline_ms ||
        channel->session->state != SessionInitialized)
        return RingInvalid;
    venus_ring_status_t result;
    if (channel->session->role == SessionHost) {
        result = send_kind(channel, ControlOffer, StopNone);
        if (result == RingOk)
            result = receive_frame(channel);
        if (result == RingOk)
            result = send_kind(channel, ControlReady, StopNone);
    } else {
        result = receive_frame(channel);
        if (result == RingOk)
            result = send_kind(channel, ControlAck, StopNone);
        if (result == RingOk)
            result = receive_frame(channel);
    }
    return result;
}

venus_ring_status_t venus_channel_wait(void *context) {
    venus_channel_t *channel = context;
    if (!channel || !channel->initialized || !channel->deadline_ms)
        return RingInvalid;
    if (channel->session->state == SessionClosed)
        return RingClosed;
    if (!venus_session_ready(channel->session))
        return RingInvalid;
    venus_ring_status_t result = receive_tick(channel);
    if (result != RingAgain)
        return result;
    uint32_t flags =
        atomic_load_explicit(&channel->session->region.commands.header->flags,
                             memory_order_acquire) |
        atomic_load_explicit(&channel->session->region.replies.header->flags, memory_order_acquire);
    if (flags) {
        venus_session_close(channel->session,
                            flags & ~VenusRingClosed ? StopProtocol : StopDisconnect);
        return flags & ~VenusRingClosed ? RingCorrupt : RingClosed;
    }
    return RingOk;
}

venus_ring_status_t venus_channel_stop(venus_channel_t *channel, venus_stop_reason_t reason) {
    if (!channel || !channel->initialized || !channel->deadline_ms)
        return RingInvalid;
    return send_kind(channel, ControlStop, reason);
}

void venus_channel_free(venus_channel_t *channel) {
    if (!channel || !channel->initialized)
        return;
    venus_session_close(channel->session, StopDisconnect);
    venus_stream_free(channel);
    memset(channel, 0, sizeof(*channel));
}
