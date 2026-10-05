/**
 * @file daemon_protocol.c
 * @brief Wire protocol serialization, framing, and validation for Waddle daemon IPC.
 */

#include "waddle/daemon_protocol.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Returns monotonic time in milliseconds.
 *
 * @return Milliseconds on success, or 0 on system clock failure.
 */
static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

int waddle_daemon_header_validate(const waddle_daemon_header_t *hdr) {
    if (hdr == NULL) {
        return -1;
    }
    if (hdr->magic != WaddleDaemonMagic) {
        return -1;
    }
    if (hdr->version != WaddleDaemonVersion) {
        return -1;
    }
    if (hdr->payload_len > WaddleDaemonMaxPayloadSize) {
        return -1;
    }
    return 0;
}

const char *waddle_subsystem_state_to_string(waddle_subsystem_state_t state) {
    switch (state) {
    case SubsystemStateStopped:
        return "stopped";
    case SubsystemStateStartingVirtiofs:
        return "starting_virtiofs";
    case SubsystemStateStartingQemu:
        return "starting_qemu";
    case SubsystemStateWaitingGuest:
        return "waiting_guest";
    case SubsystemStateRunning:
        return "running";
    case SubsystemStateStopping:
        return "stopping";
    case SubsystemStateFailed:
        return "failed";
    default:
        return "unknown";
    }
}

const char *waddle_daemon_msg_type_to_string(waddle_daemon_msg_type_t type) {
    switch (type) {
    case DaemonMsgNone:
        return "none";
    case DaemonMsgStartReq:
        return "start_req";
    case DaemonMsgStartResp:
        return "start_resp";
    case DaemonMsgStopReq:
        return "stop_req";
    case DaemonMsgStopResp:
        return "stop_resp";
    case DaemonMsgStatusReq:
        return "status_req";
    case DaemonMsgStatusResp:
        return "status_resp";
    case DaemonMsgKillReq:
        return "kill_req";
    case DaemonMsgKillResp:
        return "kill_resp";
    case DaemonMsgFsListReq:
        return "fs_list_req";
    case DaemonMsgFsListResp:
        return "fs_list_resp";
    case DaemonMsgLogsReq:
        return "logs_req";
    case DaemonMsgLogsResp:
        return "logs_resp";
    case DaemonMsgErrorResp:
        return "error_resp";
    default:
        return "unknown";
    }
}

/**
 * @brief Sends an exact byte buffer over a socket with optional timeout.
 *
 * @param[in] fd         Socket descriptor.
 * @param[in] buf        Pointer to buffer.
 * @param[in] len        Total bytes to send.
 * @param[in] timeout_ms Timeout in milliseconds (0 for indefinite).
 * @return 0 on success, or -1 on error / timeout.
 */
static int write_exact(int fd, const void *buf, size_t len, uint32_t timeout_ms) {
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t written = 0;
    uint64_t deadline_ms = 0;
    if (timeout_ms > 0) {
        deadline_ms = get_monotonic_ms() + timeout_ms;
    }

    while (written < len) {
        int poll_timeout = -1;
        if (deadline_ms > 0) {
            uint64_t now = get_monotonic_ms();
            if (now >= deadline_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            uint64_t left = deadline_ms - now;
            poll_timeout = (left > (uint64_t)INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd pfd = {fd, POLLOUT, 0};
        int pr = poll(&pfd, 1, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        ssize_t n = write(fd, ptr + written, len - written);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/**
 * @brief Reads an exact byte buffer from a socket with optional timeout.
 *
 * @param[in]  fd         Socket descriptor.
 * @param[out] buf        Destination buffer.
 * @param[in]  len        Total bytes to read.
 * @param[in]  timeout_ms Timeout in milliseconds (0 for indefinite).
 * @return 0 on success, -1 on error/timeout, or -2 on clean EOF before any bytes read.
 */
static int read_exact(int fd, void *buf, size_t len, uint32_t timeout_ms) {
    uint8_t *ptr = (uint8_t *)buf;
    size_t read_bytes = 0;
    uint64_t deadline_ms = 0;
    if (timeout_ms > 0) {
        deadline_ms = get_monotonic_ms() + timeout_ms;
    }

    while (read_bytes < len) {
        int poll_timeout = -1;
        if (deadline_ms > 0) {
            uint64_t now = get_monotonic_ms();
            if (now >= deadline_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            uint64_t left = deadline_ms - now;
            poll_timeout = (left > (uint64_t)INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        ssize_t n = read(fd, ptr + read_bytes, len - read_bytes);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            if (read_bytes == 0) {
                return -2; /* Clean EOF */
            }
            errno = EIO; /* Unexpected EOF during frame read */
            return -1;
        }
        read_bytes += (size_t)n;
    }
    return 0;
}

int waddle_daemon_send_msg(int fd,
                           uint16_t msg_type,
                           uint32_t sequence,
                           const void *payload,
                           uint32_t payload_len) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (payload_len > WaddleDaemonMaxPayloadSize) {
        errno = EMSGSIZE;
        return -1;
    }
    if (payload_len > 0 && payload == NULL) {
        errno = EINVAL;
        return -1;
    }

    waddle_daemon_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WaddleDaemonMagic;
    hdr.version = WaddleDaemonVersion;
    hdr.msg_type = msg_type;
    hdr.sequence = sequence;
    hdr.payload_len = payload_len;

    /* Write 16-byte header */
    if (write_exact(fd, &hdr, sizeof(hdr), 0) != 0) {
        return -1;
    }

    /* Write payload if present */
    if (payload_len > 0) {
        if (write_exact(fd, payload, payload_len, 0) != 0) {
            return -1;
        }
    }

    return 0;
}

int waddle_daemon_recv_msg(int fd,
                           waddle_daemon_header_t *hdr,
                           void *payload_buf,
                           uint32_t payload_buf_len,
                           uint32_t timeout_ms) {
    if (fd < 0 || hdr == NULL) {
        errno = (fd < 0) ? EBADF : EINVAL;
        return -1;
    }

    /* Read header */
    int r = read_exact(fd, hdr, sizeof(waddle_daemon_header_t), timeout_ms);
    if (r != 0) {
        return r;
    }

    /* Validate header */
    if (waddle_daemon_header_validate(hdr) != 0) {
        errno = EPROTO;
        return -1;
    }

    if (hdr->payload_len == 0) {
        return 0;
    }

    /* Check buffer capacity */
    if (hdr->payload_len > payload_buf_len) {
        errno = ENOBUFS;
        return -1;
    }
    if (payload_buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Read payload */
    r = read_exact(fd, payload_buf, hdr->payload_len, timeout_ms);
    if (r != 0) {
        return (r == -2) ? -1 : r; /* Truncated frame is an error */
    }

    return 0;
}
