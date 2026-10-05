/** @file venus_stream_linux.c @brief Nonblocking VSOCK/UNIX lifecycle I/O. */
#include "venus_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

venus_ring_status_t venus_stream_open(venus_channel_t *channel) {
    if (channel->stream < 0 || channel->stream > INT_MAX)
        return RingInvalid;
    int descriptor = (int)channel->stream;
    int flags = fcntl(descriptor, F_GETFL);
    int type = 0;
    socklen_t length = sizeof(type);
    if (flags < 0 || !(flags & O_NONBLOCK) ||
        getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM)
        return RingInvalid;
    return RingOk;
}

void venus_stream_free(venus_channel_t *channel) { channel->event = 0; }

uint64_t venus_stream_time_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 1000) / 1000)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000 + 1;
}

venus_ring_status_t venus_stream_io(venus_channel_t *channel, void *buffer, size_t length,
                                    int writing, size_t *transferred) {
    *transferred = 0;
    struct pollfd waiting = {(int)channel->stream, writing ? POLLOUT : POLLIN, 0};
    int ready = poll(&waiting, 1, 1);
    if (ready == 0 || (ready < 0 && errno == EINTR))
        return RingAgain;
    if (ready < 0 || waiting.revents & (POLLERR | POLLNVAL))
        return RingClosed;
    ssize_t count = writing ? send(waiting.fd, buffer, length, MSG_DONTWAIT | MSG_NOSIGNAL)
                            : recv(waiting.fd, buffer, length, MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return RingAgain;
    if (count <= 0)
        return RingClosed;
    *transferred = (size_t)count;
    return RingOk;
}
