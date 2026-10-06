/** @file venus_frame_linux.c @brief Nonblocking credential-bound native FD transport. */
#include "waddle/venus_frame.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

venus_ring_status_t venus_frame_prepare(int fd) {
    int type, domain, enabled = 1;
    socklen_t bytes = sizeof(type);
    int flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & O_NONBLOCK) || descriptor_flags < 0 ||
        !(descriptor_flags & FD_CLOEXEC) ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &bytes) != 0 || type != SOCK_SEQPACKET)
        return RingInvalid;
    bytes = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &bytes) != 0 || domain != AF_UNIX ||
        setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0)
        return RingInvalid;
    return RingOk;
}
void venus_frame_fds_free(int fds[4]) {
    if (!fds)
        return;
    for (unsigned index = 0; index < 4; index++) {
        if (fds[index] >= 0)
            close(fds[index]);
        fds[index] = -1;
    }
}
venus_ring_status_t venus_frame_send(int socket_fd, const venus_frame_t *frame, const int *fds,
                                     size_t count) {
    unsigned char bytes[VenusFrameBytes];
    if (!fds || venus_frame_encode(frame, bytes, sizeof(bytes)) != RingOk ||
        count != frame->layout.plane_count)
        return RingInvalid;
    for (size_t index = 0; index < count; index++)
        if (fcntl(fds[index], F_GETFD) < 0)
            return RingInvalid;
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(struct ucred))];
    } control = {0};
    struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control.bytes,
                             .msg_controllen = CMSG_SPACE(count * sizeof(int)) +
                                               CMSG_SPACE(sizeof(struct ucred))};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(header), fds, count * sizeof(int));
    header = CMSG_NXTHDR(&message, header);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_CREDENTIALS;
    header->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    const struct ucred Credentials = {.pid = getpid(), .uid = getuid(), .gid = getgid()};
    memcpy(CMSG_DATA(header), &Credentials, sizeof(Credentials));
    ssize_t result = sendmsg(socket_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (result < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? RingAgain : RingClosed;
    return result == (ssize_t)sizeof(bytes) ? RingOk : RingCorrupt;
}
venus_ring_status_t venus_frame_receive(int socket_fd, int32_t expected_pid,
                                        uint64_t expected_context, venus_frame_t *frame,
                                        int fds[4]) {
    if (!frame || !fds)
        return RingInvalid;
    memset(frame, 0, sizeof(*frame));
    for (unsigned index = 0; index < 4; index++)
        fds[index] = -1;
    if (expected_pid <= 0 || !expected_context)
        return RingInvalid;
    unsigned char bytes[VenusFrameBytes];
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(struct ucred))];
    } control = {0};
    struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control.bytes,
                             .msg_controllen = sizeof(control.bytes)};
    ssize_t result = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (result < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? RingAgain : RingClosed;
    /* The kernel validates ancillary structure. A truncated rights record may
     * fill the credential space too; retain room for every actually delivered FD. */
    int acquired[16];
    size_t acquired_count = 0;
    int valid = !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
    int credentials_seen = 0, rights_seen = 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            if (rights_seen++)
                valid = 0;
            size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t index = 0; index < count; index++) {
                int fd;
                memcpy(&fd, CMSG_DATA(header) + index * sizeof(int), sizeof(fd));
                if (acquired_count < sizeof(acquired) / sizeof(acquired[0]))
                    acquired[acquired_count++] = fd;
                else {
                    close(fd);
                    valid = 0;
                }
            }
        } else if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
                   header->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred credentials;
            memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
            if (credentials_seen++ || credentials.pid != expected_pid ||
                credentials.uid != getuid())
                valid = 0;
        } else
            valid = 0;
    }
    venus_frame_t decoded;
    if (valid && credentials_seen == 1 && rights_seen == 1 &&
        venus_frame_decode(&decoded, bytes, (size_t)result) == RingOk &&
        decoded.context == expected_context && acquired_count == decoded.layout.plane_count) {
        *frame = decoded;
        for (size_t index = 0; index < acquired_count; index++)
            fds[index] = acquired[index];
        return RingOk;
    }
    for (size_t index = 0; index < acquired_count; index++)
        close(acquired[index]);
    return result == 0 ? RingClosed : RingCorrupt;
}
