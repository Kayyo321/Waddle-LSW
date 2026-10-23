/** @file frame.c @brief Real seqpacket descriptor transfer, corruption and syscall faults. */
#include "waddle/venus_frame.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static int prepare_fault, send_fault, receive_fault;
static int fixture_getsockopt(int fd, int level, int option, void *value, socklen_t *bytes) {
    if (prepare_fault == 1 && option == SO_TYPE)
        return -1;
    if (prepare_fault == 2 && option == SO_DOMAIN)
        return -1;
    int result = getsockopt(fd, level, option, value, bytes);
    if (prepare_fault == 3 && option == SO_DOMAIN)
        *(int *)value = AF_INET;
    return result;
}
static int fixture_setsockopt(int fd, int level, int option, const void *value, socklen_t bytes) {
    return prepare_fault == 4 ? -1 : setsockopt(fd, level, option, value, bytes);
}
static ssize_t fixture_sendmsg(int fd, const struct msghdr *message, int flags) {
    if (send_fault) {
        errno = send_fault == 1 ? EAGAIN : send_fault == 2 ? EINTR : EIO;
        return send_fault == 4 ? 1 : -1;
    }
    return sendmsg(fd, message, flags);
}
static ssize_t fixture_recvmsg(int fd, struct msghdr *message, int flags) {
    if (receive_fault == 1 || receive_fault == 2 || receive_fault == 3) {
        errno = receive_fault == 1 ? EAGAIN : receive_fault == 2 ? EINTR : EIO;
        return -1;
    }
    ssize_t result = recvmsg(fd, message, flags);
    if (result >= 0 && receive_fault == 4) {
        for (struct cmsghdr *header = CMSG_FIRSTHDR(message); header;
             header = CMSG_NXTHDR(message, header))
            if (header->cmsg_type == SCM_CREDENTIALS) {
                struct ucred credentials;
                memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
                credentials.uid++;
                memcpy(CMSG_DATA(header), &credentials, sizeof(credentials));
            }
    }
    if (result >= 0 && receive_fault == 5) {
        for (struct cmsghdr *header = CMSG_FIRSTHDR(message); header;
             header = CMSG_NXTHDR(message, header))
            if (header->cmsg_type == SCM_CREDENTIALS)
                header->cmsg_type = 1234;
    }
    if (result >= 0 && receive_fault == 6) {
        struct cmsghdr *header = CMSG_FIRSTHDR(message);
        assert(header && header->cmsg_type == SCM_CREDENTIALS);
        size_t extent = CMSG_SPACE(sizeof(struct ucred));
        memcpy((unsigned char *)message->msg_control + extent, header, extent);
        message->msg_controllen = extent * 2;
    }
    if (result >= 0 && receive_fault == 7) {
        struct cmsghdr *header = CMSG_FIRSTHDR(message);
        assert(header && header->cmsg_type == SCM_CREDENTIALS);
        header->cmsg_len--;
    }
    return result;
}
#define getsockopt fixture_getsockopt
#define setsockopt fixture_setsockopt
#define sendmsg fixture_sendmsg
#define recvmsg fixture_recvmsg
#include "venus_frame_linux.c"
#undef sendmsg
#undef recvmsg
#undef setsockopt
#undef getsockopt

static unsigned descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory))
        count++;
    assert(!closedir(directory));
    return count;
}
static venus_frame_t example(void) {
    venus_frame_t frame = {.context = 17,
                           .frame = 1,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.width = 32, .height = 16}}};
    return frame;
}
static void raw_send(int socket_fd, const void *bytes, size_t length, int fd, unsigned count) {
    unsigned char control[CMSG_SPACE(sizeof(int) * 16)] = {0};
    struct iovec vector = {.iov_base = (void *)bytes, .iov_len = length};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    if (count) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(count * sizeof(int));
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(count * sizeof(int));
        for (unsigned index = 0; index < count; index++)
            memcpy(CMSG_DATA(header) + index * sizeof(int), &fd, sizeof(fd));
    }
    assert(sendmsg(socket_fd, &message, MSG_NOSIGNAL) == (ssize_t)length);
}
static void releases(int sockets[2], int source) {
    venus_release_t release = {.context = 17, .frame = 1, .status = RingOk}, decoded;
    assert(venus_release_send(sockets[0], NULL) == RingInvalid);
    assert(venus_release_receive(sockets[1], getpid(), 17, NULL) == RingInvalid);
    assert(venus_release_receive(sockets[1], 0, 17, &decoded) == RingInvalid);
    assert(venus_release_receive(sockets[1], getpid(), 0, &decoded) == RingInvalid);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingAgain);
    for (send_fault = 1; send_fault <= 4; send_fault++) {
        venus_ring_status_t expected = send_fault < 3    ? RingAgain
                                       : send_fault == 3 ? RingClosed
                                                         : RingCorrupt;
        assert(venus_release_send(sockets[0], &release) == expected);
    }
    send_fault = 0;
    for (receive_fault = 1; receive_fault <= 3; receive_fault++)
        assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) ==
               (receive_fault < 3 ? RingAgain : RingClosed));
    receive_fault = 0;
    unsigned baseline = descriptors();
    const venus_ring_status_t Statuses[] = {RingOk, RingInvalid, RingCancelled, RingClosed};
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        release.frame++;
        release.status = Statuses[iteration % 4];
        assert(venus_release_send(sockets[0], &release) == RingOk);
        assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingOk);
        assert(decoded.frame == release.frame && decoded.status == release.status);
        assert(descriptors() == baseline);
    }
    /* Exercise actual kernel backpressure and exact caller-owned retry. */
    int send_bytes = 4096;
    assert(!setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)));
    unsigned queued = 0;
    venus_ring_status_t queued_status;
    while ((queued_status = venus_release_send(sockets[0], &release)) == RingOk) {
        queued++;
        assert(queued < 4096);
    }
    assert(queued && queued_status == RingAgain);
    for (unsigned index = 0; index < queued; index++) {
        assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingOk);
        assert(decoded.frame == release.frame && decoded.status == release.status);
    }
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingAgain);
    assert(venus_release_send(sockets[0], &release) == RingOk);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingOk);
    assert(descriptors() == baseline);
    for (int mode = 0; mode < 6; mode++) {
        assert(venus_release_send(sockets[0], &release) == RingOk);
        receive_fault = mode >= 2 ? mode + 2 : 0;
        assert(venus_release_receive(sockets[1], mode == 0 ? getpid() + 1 : getpid(),
                                     mode == 1 ? 18 : 17, &decoded) == RingCorrupt);
        assert(!decoded.context && !decoded.frame && descriptors() == baseline);
    }
    receive_fault = 0;
    unsigned char bytes[VenusReleaseBytes + 1];
    assert(venus_release_encode(&release, bytes, VenusReleaseBytes) == RingOk);
    for (unsigned count = 1; count <= 16; count++) {
        raw_send(sockets[0], bytes, VenusReleaseBytes, source, count);
        assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingCorrupt);
        assert(descriptors() == baseline && !decoded.context);
    }
    raw_send(sockets[0], bytes, 12, source, 0);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingCorrupt);
    raw_send(sockets[0], bytes, sizeof(bytes), source, 0);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingCorrupt);
    bytes[28] = 1;
    raw_send(sockets[0], bytes, VenusReleaseBytes, source, 0);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingCorrupt);
    bytes[28] = 0;
    int disabled = 0;
    assert(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &disabled, sizeof(disabled)));
    raw_send(sockets[0], bytes, VenusReleaseBytes, source, 0);
    assert(venus_release_receive(sockets[1], getpid(), 17, &decoded) == RingCorrupt);
    assert(venus_frame_prepare(sockets[1]) == RingOk);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(sockets[1]);
        assert(venus_release_send(sockets[0], &release) == RingOk);
        close(sockets[0]);
        close(source);
        _exit(0);
    }
    venus_ring_status_t status;
    unsigned attempts = 0;
    do {
        status = venus_release_receive(sockets[1], child, 17, &decoded);
        usleep(1000);
        assert(++attempts < 5000);
    } while (status == RingAgain);
    assert(status == RingOk && decoded.frame == release.frame);
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
           !WEXITSTATUS(child_status));
    assert(descriptors() == baseline);
}
int main(void) {
    unsigned initial = descriptors();
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets));
    assert(venus_frame_prepare(-1) == RingInvalid);
    assert(!fcntl(sockets[0], F_SETFL, 0));
    assert(venus_frame_prepare(sockets[0]) == RingInvalid);
    assert(!fcntl(sockets[0], F_SETFL, O_NONBLOCK));
    assert(!fcntl(sockets[0], F_SETFD, 0));
    assert(venus_frame_prepare(sockets[0]) == RingInvalid);
    assert(!fcntl(sockets[0], F_SETFD, FD_CLOEXEC));
    int streams[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, streams));
    assert(venus_frame_prepare(streams[0]) == RingInvalid);
    close(streams[0]);
    close(streams[1]);
    for (prepare_fault = 1; prepare_fault <= 4; prepare_fault++)
        assert(venus_frame_prepare(sockets[0]) == RingInvalid);
    prepare_fault = 0;
    assert(venus_frame_prepare(sockets[0]) == RingOk);
    assert(venus_frame_prepare(sockets[1]) == RingOk);
    int source = memfd_create("dma-buf-fixture", MFD_CLOEXEC);
    assert(source >= 0 && !ftruncate(source, 4096));
    releases(sockets, source);
    venus_frame_t frame = example(), decoded;
    int output[4];
    assert(venus_frame_receive(sockets[1], getpid(), 17, NULL, output) == RingInvalid);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, NULL) == RingInvalid);
    assert(venus_frame_receive(sockets[1], 0, 17, &decoded, output) == RingInvalid);
    assert(venus_frame_receive(sockets[1], getpid(), 0, &decoded, output) == RingInvalid);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingAgain);
    for (receive_fault = 1; receive_fault <= 3; receive_fault++) {
        venus_ring_status_t expected = receive_fault < 3 ? RingAgain : RingClosed;
        assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == expected);
    }
    receive_fault = 0;
    assert(venus_frame_send(sockets[0], NULL, &source, 1) == RingInvalid);
    assert(venus_frame_send(sockets[0], &frame, NULL, 1) == RingInvalid);
    assert(venus_frame_send(sockets[0], &frame, &source, 0) == RingInvalid);
    int invalid = -1;
    assert(venus_frame_send(sockets[0], &frame, &invalid, 1) == RingInvalid);
    for (send_fault = 1; send_fault <= 4; send_fault++) {
        venus_ring_status_t expected = send_fault < 3    ? RingAgain
                                       : send_fault == 3 ? RingClosed
                                                         : RingCorrupt;
        assert(venus_frame_send(sockets[0], &frame, &source, 1) == expected);
    }
    send_fault = 0;
    unsigned baseline = descriptors();
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        frame.frame++;
        assert(venus_frame_send(sockets[0], &frame, &source, 1) == RingOk);
        assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingOk);
        assert(decoded.frame == frame.frame && output[0] >= 0 && output[1] == -1);
        assert(fcntl(output[0], F_GETFD) & FD_CLOEXEC);
        const unsigned char Token = 0xa5;
        assert(pwrite(source, &Token, 1, 0) == 1);
        unsigned char token = 0;
        assert(pread(output[0], &token, 1, 0) == 1 && token == Token);
        venus_frame_fds_free(output);
        venus_frame_fds_free(output);
        assert(fcntl(source, F_GETFD) >= 0 && descriptors() == baseline);
    }
    venus_frame_fds_free(NULL);
    frame.layout.fourcc = 0x3231564e;
    frame.layout.plane_count = 2;
    frame.layout.planes[0] = (venus_dmabuf_plane_t){.stride = 32, .size = 512, .extent = 4096};
    frame.layout.planes[1] =
        (venus_dmabuf_plane_t){.offset = 512, .stride = 32, .size = 256, .extent = 4096};
    frame.resource_ids[1] = 3;
    int planes[2] = {source, source};
    assert(venus_frame_send(sockets[0], &frame, planes, 2) == RingOk);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingOk);
    assert(output[0] >= 0 && output[1] >= 0 && output[0] != output[1]);
    assert((fcntl(output[0], F_GETFD) & FD_CLOEXEC) && (fcntl(output[1], F_GETFD) & FD_CLOEXEC));
    venus_frame_fds_free(output);
    assert(descriptors() == baseline);
    frame = example();
    for (int mode = 0; mode < 4; mode++) {
        assert(venus_frame_send(sockets[0], &frame, &source, 1) == RingOk);
        receive_fault = mode == 2 ? 4 : mode == 3 ? 5 : 0;
        assert(venus_frame_receive(sockets[1], mode == 0 ? getpid() + 1 : getpid(),
                                   mode == 1 ? 18 : 17, &decoded, output) == RingCorrupt);
        assert(!decoded.context && output[0] == -1 && descriptors() == baseline);
    }
    receive_fault = 0;
    unsigned char bytes[VenusFrameBytes + 1];
    assert(venus_frame_encode(&frame, bytes, VenusFrameBytes) == RingOk);
    for (unsigned count = 0; count <= 16; count++) {
        if (count == 1)
            continue;
        raw_send(sockets[0], bytes, VenusFrameBytes, source, count);
        assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingCorrupt);
        assert(descriptors() == baseline && output[0] == -1);
    }
    raw_send(sockets[0], bytes, 12, source, 1);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingCorrupt);
    raw_send(sockets[0], bytes, sizeof(bytes), source, 1);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingCorrupt);
    bytes[44] = 1;
    raw_send(sockets[0], bytes, VenusFrameBytes, source, 1);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingCorrupt);
    bytes[44] = 0;
    int disabled = 0;
    assert(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &disabled, sizeof(disabled)));
    raw_send(sockets[0], bytes, VenusFrameBytes, source, 1);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingCorrupt);
    assert(venus_frame_prepare(sockets[1]) == RingOk);
    // Kernel credentials follow the exec/fork sender, not socketpair creator identity.
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(sockets[1]);
        assert(venus_frame_send(sockets[0], &frame, &source, 1) == RingOk);
        close(sockets[0]);
        close(source);
        _exit(0);
    }
    venus_ring_status_t status;
    unsigned attempts = 0;
    do {
        status = venus_frame_receive(sockets[1], child, 17, &decoded, output);
        usleep(1000);
        assert(++attempts < 5000);
    } while (status == RingAgain);
    assert(status == RingOk && output[0] >= 0);
    venus_frame_fds_free(output);
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
           !WEXITSTATUS(child_status));
    assert(descriptors() == baseline);
    close(sockets[0]);
    assert(venus_frame_receive(sockets[1], getpid(), 17, &decoded, output) == RingClosed);
    assert(venus_frame_send(sockets[1], &frame, &source, 1) == RingClosed);
    venus_release_t release = {.context = 17, .frame = 1, .status = RingOk}, completed;
    assert(venus_release_receive(sockets[1], getpid(), 17, &completed) == RingClosed);
    assert(venus_release_send(sockets[1], &release) == RingClosed);
    close(sockets[1]);
    close(source);
    assert(descriptors() == initial);
    return 0;
}
