/** @file av_host.c @brief Native Wayland/PipeWire client for a managed AV guest. */
#include "av_dmabuf.h"
#include "av_kvmfr.h"
#include <sys/ioctl.h>
#include "av_layout.h"
#include "av_peer.h"
#include "av_pipewire.h"
#include "av_wayland.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
/** @brief Main-loop-owned borrowed mappings and native clients. */
typedef struct host_av_t {
    av_peer_t peer;
    av_wayland_t *video;
    av_pipewire_t audio;
    void *mapping;
    int memory_fd;
} host_av_t;
static volatile sig_atomic_t stopping;
static void stop_signal(int signal_number) {
    (void)signal_number;
    stopping = 1;
}
static int host_request(const av_message_t *message, void *context) {
    host_av_t *host = context;
    return av_peer_send(&host->peer, message) == 0 ? 0 : -1;
}
static int guest_message(const av_message_t *message, void *context) {
    host_av_t *host = context;
    if (message->type == MsgWindowCreate) {
        unsigned pool = message->buffer_index;
        if (pool >= AvMaxWindows)
            return -1;
        window_slot_header_t *slots[AvVideoBuffers];
        uint64_t offsets[AvVideoBuffers];
        for (unsigned index = 0; index < AvVideoBuffers; ++index) {
            slots[index] = av_layout_slot(host->mapping, AvMappingBytes, pool, index);
            offsets[index] = av_layout_pixels(pool, index);
        }
        return av_wayland_create(host->video, message, host->memory_fd, AvUsedBytes, slots, offsets,
                                 AvSlotCapacity);
    }
    return av_wayland_message(host->video, message);
}
/** @brief Run native AV playback for the specified managed guest/mapping.
 * @param[in] argc CRT argument count. @param[in] argv Borrowed NUL-terminated args.
 * @return 0 peer closure/help, 2 invalid arguments, 1 startup/transport/display error.
 * @note Main owns socket/mapping/Wayland; PipeWire stops before mapping release.
 * Does not create another VM or initialize a mapping while guest is active.
 */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("waddle-av-host GUEST_CID SHARED_MEMORY_PATH | --probe SHARED_MEMORY_PATH");
        return 0;
    }
    uint32_t cid = 0;
    int probe = argc == 3 && !strcmp(argv[1], "--probe");
    if (argc != 3 || (!probe && (av_number_parse(argv[1], strlen(argv[1]), &cid) != 0 || cid < 3)))
        return 2;
    host_av_t host = {.memory_fd = -1};
    int peer = -1, result = 1;
    host.memory_fd = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (host.memory_fd < 0)
        goto cleanup;
    struct stat status;
    if (fstat(host.memory_fd, &status) != 0 ||
        (!S_ISCHR(status.st_mode) &&
         (!S_ISREG(status.st_mode) || status.st_size != (off_t)AvMappingBytes)))
        goto cleanup;
    if (S_ISCHR(status.st_mode) && av_kvmfr_size(host.memory_fd) != (long)AvMappingBytes) {
        errno = EINVAL;
        goto cleanup;
    }
    void *mapping =
        mmap(NULL, AvMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, host.memory_fd, 0);
    if (mapping == MAP_FAILED)
        goto cleanup;
    host.mapping = mapping;
    if (av_layout_validate(mapping, AvMappingBytes) != 0)
        goto cleanup;
    if (probe) goto start_clients;
    peer = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (peer < 0)
        goto cleanup;
    struct sockaddr_vm address = {
        .svm_family = AF_VSOCK, .svm_cid = cid, .svm_port = AvControlPort};
    if (connect(peer, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS)
            goto cleanup;
        struct pollfd connecting = {peer, POLLOUT, 0};
        int ready = poll(&connecting, 1, 5000), socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (ready <= 0) {
            if (!ready) errno = ETIMEDOUT;
            goto cleanup;
        }
        if (getsockopt(peer, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0)
            goto cleanup;
        if (socket_error) {
            errno = socket_error;
            goto cleanup;
        }
    }
start_clients:
    host.peer.socket = (uintptr_t)peer;
    if (av_wayland_init(&host.video, host_request, &host) != 0)
        goto cleanup;
    uint8_t *base = host.mapping;
    if (av_pipewire_init(&host.audio, (audio_ring_header_t *)(base + AvAudioHeaderOffset),
                         base + AvPcmOffset, AvAudioCapacity * AvAudioFrameBytes) != 0)
        goto cleanup;
    if (probe) {
        puts("AV host probe: shared mapping, Wayland and connected PipeWire ready");
        result = 0;
        goto cleanup;
    }
    struct sigaction action = {.sa_handler = stop_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    result = 0;
    while (!stopping) {
        if (!av_pipewire_ready(&host.audio)) {
            errno = ENOTCONN;
            result = 1;
            break;
        }
        struct pollfd descriptors[2] = {
            {peer, POLLIN | (host.peer.head != host.peer.tail ? POLLOUT : 0), 0},
            {av_wayland_fd(host.video), POLLIN | (av_wayland_writable(host.video) ? POLLOUT : 0), 0}};
        int ready = poll(descriptors, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            result = 1;
            break;
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            result = 1;
            break;
        }
        if (descriptors[1].revents & POLLIN && av_wayland_dispatch(host.video) != 0) {
            result = 1;
            break;
        }
        if (av_wayland_flush(host.video) != 0) {
            result = 1;
            break;
        }
        int peer_status = av_peer_pump(&host.peer, guest_message, &host);
        if (peer_status != 0) {
            result = peer_status == 1 ? 0 : 1;
            break;
        }
    }
cleanup:
    if (result)
        fprintf(stderr, "AV host: session setup/playback failed: %s (%d)\n", strerror(errno),
                errno);
    if (peer >= 0) {
        shutdown(peer, SHUT_RDWR);
        close(peer);
    }
    av_pipewire_free(&host.audio);
    av_wayland_free(&host.video);
    if (host.mapping)
        munmap(host.mapping, AvMappingBytes);
    if (host.memory_fd >= 0)
        close(host.memory_fd);
    return result;
}
