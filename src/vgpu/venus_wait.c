#include "waddle/venus_wait.h"

static venus_ring_status_t wait_for_progress(venus_ring_t *ring, venus_wait_callback_t wait,
                                             void *context) {
    venus_ring_status_t status = wait(context);
    switch (status) {
    case RingOk:
    case RingCancelled:
    case RingTimeout:
    case RingCorrupt:
    case RingInvalid:
        return status;
    case RingClosed:
        venus_ring_close(ring);
        return RingClosed;
    default:
        return RingInvalid;
    }
}

venus_ring_status_t venus_ring_write_wait(venus_ring_t *ring, const void *data, size_t length,
                                          venus_wait_callback_t wait, void *context) {
    if (!wait)
        return RingInvalid;
    for (;;) {
        venus_ring_status_t status = venus_ring_write(ring, data, length);
        if (status != RingAgain)
            return status;
        status = wait_for_progress(ring, wait, context);
        if (status != RingOk)
            return status;
    }
}

venus_ring_status_t venus_ring_read_wait(venus_ring_t *ring, void *data, size_t length,
                                         venus_wait_callback_t wait, void *context) {
    if (!wait)
        return RingInvalid;
    for (;;) {
        venus_ring_status_t status = venus_ring_read(ring, data, length);
        if (status != RingAgain)
            return status;
        status = wait_for_progress(ring, wait, context);
        if (status != RingOk)
            return status;
    }
}
