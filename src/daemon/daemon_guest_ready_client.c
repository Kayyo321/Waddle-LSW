/** @file daemon_guest_ready_client.c @brief Bounded correlated readiness transaction. */
#include "daemon_client.h"
#include "daemon_wire.h"
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static int monotonic_ms(uint64_t *out_ms) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L ||
        (uint64_t)value.tv_sec > (UINT64_MAX - 122999) / 1000) {
        errno = EOVERFLOW;
        return -1;
    }
    *out_ms = (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
    return 0;
}

static int transfer_exact(int fd, uint8_t *bytes, size_t count, int writing,
                           uint64_t deadline_ms) {
    size_t offset = 0;
    while (offset < count) {
        uint64_t now_ms;
        if (monotonic_ms(&now_ms) != 0) return -1;
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        ssize_t moved = writing ? send(fd, bytes + offset, count - offset, MSG_NOSIGNAL | MSG_DONTWAIT) :
            recv(fd, bytes + offset, count - offset, MSG_DONTWAIT);
        if (moved > 0) { offset += (size_t)moved; continue; }
        if (moved == 0) { errno = ECONNRESET; return -1; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        struct pollfd descriptor = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        /* Refresh after the syscall: one shared absolute deadline, never a new interval. */
        if (monotonic_ms(&now_ms) != 0) return -1;
        if (now_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
        int result = poll(&descriptor, 1, (int)(deadline_ms - now_ms));
        if (result < 0) { if (errno == EINTR) continue; return -1; }
        if (result == 0) { errno = ETIMEDOUT; return -1; }
        if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
        /* recv/send decides whether bytes remain when HUP accompanies readiness. */
    }
    uint64_t completed_ms;
    if (monotonic_ms(&completed_ms) != 0) return -1;
    if (completed_ms >= deadline_ms) { errno = ETIMEDOUT; return -1; }
    return 0;
}

int waddle_client_guest_ready(int fd, uint32_t timeout_ms,
                              waddle_daemon_guest_ready_resp_t *resp) {
    if (fd < 0 || timeout_ms > 120000 || resp == NULL) { errno = EINVAL; return -1; }
    static _Atomic uint32_t next_sequence = UINT32_C(0x47520000);
    uint32_t sequence = atomic_fetch_add_explicit(&next_sequence, 1, memory_order_relaxed);
    uint64_t now_ms;
    if (monotonic_ms(&now_ms) != 0) return -1;
    uint64_t deadline_ms = now_ms + (timeout_ms == 0 ? 120000 : timeout_ms) + 2000;
    waddle_daemon_header_t request = {.magic = WaddleDaemonMagic, .version = WaddleDaemonVersion,
        .msg_type = DaemonMsgGuestReadyReq, .sequence = sequence, .payload_len = 4};
    uint8_t frame[WaddleDaemonHeaderSize + sizeof(timeout_ms)];
    memcpy(frame, &request, sizeof(request));
    memcpy(frame + sizeof(request), &timeout_ms, sizeof(timeout_ms));
    if (transfer_exact(fd, frame, sizeof(frame), 1, deadline_ms) != 0) return -1;
    uint8_t header_bytes[WaddleDaemonHeaderSize];
    waddle_daemon_header_t header;
    if (transfer_exact(fd, header_bytes, sizeof(header_bytes), 0, deadline_ms) != 0) return -1;
    if (daemon_wire_decode_header(header_bytes, sizeof(header_bytes), &header) != 0 ||
        header.sequence != sequence ||
        !((header.msg_type == DaemonMsgGuestReadyResp && header.payload_len == 8) ||
          (header.msg_type == DaemonMsgErrorResp && header.payload_len == 264))) {
        errno = EPROTO;
        return -1;
    }
    uint8_t payload[264];
    if (transfer_exact(fd, payload, header.payload_len, 0, deadline_ms) != 0) return -1;
    if (header.msg_type == DaemonMsgErrorResp) {
        uint32_t code;
        if (daemon_wire_decode_error_response(payload, header.payload_len, &code) != 0) errno = EPROTO;
        else errno = (int)code;
        return -1;
    }
    waddle_daemon_guest_ready_resp_t result;
    if (daemon_wire_decode_ready_response(payload, header.payload_len, &result) != 0) {
        errno = EPROTO;
        return -1;
    }
    *resp = result;
    return 0;
}
