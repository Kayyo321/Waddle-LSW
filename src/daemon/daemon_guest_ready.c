/** @file daemon_guest_ready.c @brief One-fd nonblocking configured guest-exec probe. */
#include "daemon_guest_ready.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <unistd.h>

static void close_probe(daemon_guest_ready_t *probe) {
    int fd = probe->probe_fd;
    probe->probe_fd = -1;
    if (fd >= 0) (void)close(fd);
}

void daemon_guest_ready_init(daemon_guest_ready_t *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->probe_fd = -1;
}

void daemon_guest_ready_cancel(daemon_guest_ready_t *probe) {
    close_probe(probe);
    probe->active = 0;
}

int daemon_guest_ready_start(daemon_guest_ready_t *probe, uint32_t cid,
                             uint32_t port, uint32_t timeout_ms,
                             uint64_t now_ms, uint32_t sequence) {
    if (probe == NULL || cid < 3 || cid == UINT32_MAX || port == 0 ||
        timeout_ms == 0 || timeout_ms > 120000 || now_ms > UINT64_MAX - timeout_ms) {
        errno = EINVAL;
        return -1;
    }
    if (probe->active || probe->probe_fd != -1) {
        errno = EBUSY;
        return -1;
    }
    *probe = (daemon_guest_ready_t){.probe_fd = -1, .active = 1,
        .sequence = sequence, .cid = cid, .port = port,
        .deadline_ms = now_ms + timeout_ms, .retry_at_ms = now_ms};
    return 0;
}

static int complete_probe(daemon_guest_ready_t *probe, int32_t status,
                          waddle_daemon_guest_ready_resp_t *response) {
    close_probe(probe);
    probe->active = 0;
    *response = (waddle_daemon_guest_ready_resp_t){.status = status, .cid = probe->cid};
    return 1;
}

static void retry_probe(daemon_guest_ready_t *probe, uint64_t now_ms) {
    close_probe(probe);
    uint64_t remaining = probe->deadline_ms - now_ms;
    probe->retry_at_ms = now_ms + (remaining < 250 ? remaining : 250);
}

int daemon_guest_ready_step(daemon_guest_ready_t *probe, int running,
                            uint64_t now_ms, short revents,
                            waddle_daemon_guest_ready_resp_t *response) {
    if (!probe->active) return 0;
    if (!running) return complete_probe(probe, -ENOTCONN, response);
    if (now_ms >= probe->deadline_ms) return complete_probe(probe, -ETIMEDOUT, response);
    if (probe->probe_fd >= 0) {
        if (now_ms >= probe->probe_deadline_ms) {
            retry_probe(probe, now_ms);
        } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            retry_probe(probe, now_ms);
        } else if (revents & POLLOUT) {
            int socket_error = 0;
            socklen_t bytes = sizeof(socket_error);
            if (getsockopt(probe->probe_fd, SOL_SOCKET, SO_ERROR, &socket_error, &bytes) == 0 &&
                bytes == sizeof(socket_error) && socket_error == 0)
                return complete_probe(probe, 0, response);
            retry_probe(probe, now_ms);
        }
        return 0;
    }
    if (now_ms < probe->retry_at_ms) return 0;
    probe->probe_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe->probe_fd < 0) {
        retry_probe(probe, now_ms);
        return 0;
    }
    struct sockaddr_vm address = {.svm_family = AF_VSOCK,
        .svm_cid = probe->cid, .svm_port = probe->port};
    if (connect(probe->probe_fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        return complete_probe(probe, 0, response);
    if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
        retry_probe(probe, now_ms);
        return 0;
    }
    uint64_t remaining = probe->deadline_ms - now_ms;
    probe->probe_deadline_ms = now_ms + (remaining < 2000 ? remaining : 2000);
    return 0;
}
