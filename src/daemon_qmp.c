/**
 * @file daemon_qmp.c
 * @brief Implementation of lightweight QEMU Machine Protocol (QMP) JSON-RPC client.
 */

#include "daemon_qmp.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Returns monotonic time in milliseconds.
 *
 * @return Milliseconds on success, or 0 on system clock failure.
 */
static uint64_t qmp_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

/**
 * @brief Reads one newline-delimited JSON line from the QMP socket within a timeout.
 *
 * @param[in]  fd         Socket descriptor.
 * @param[out] buf        Destination buffer.
 * @param[in]  buf_cap    Capacity of destination buffer.
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return Length of line read on success, -1 on error/timeout, or -2 on EOF.
 */
static ssize_t qmp_read_line(int fd, char *buf, size_t buf_cap, uint32_t timeout_ms) {
    if (fd < 0 || buf == NULL || buf_cap < 2) {
        errno = EINVAL;
        return -1;
    }

    uint64_t deadline = (timeout_ms > 0) ? (qmp_monotonic_ms() + timeout_ms) : 0;
    size_t pos = 0;

    while (pos + 1 < buf_cap) {
        int poll_timeout = -1;
        if (deadline > 0) {
            uint64_t now = qmp_monotonic_ms();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            uint64_t left = deadline - now;
            poll_timeout = (left > (uint64_t)INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (n == 0) {
            if (pos == 0) return -2; /* EOF */
            break;
        }

        buf[pos++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buf[pos] = '\0';
    return (ssize_t)pos;
}

/**
 * @brief Writes exact buffer to socket within a timeout.
 *
 * @param[in] fd         Socket descriptor.
 * @param[in] str        String to write.
 * @param[in] len        Number of bytes to write.
 * @param[in] timeout_ms Timeout in milliseconds.
 * @return 0 on success, or -1 on error/timeout.
 */
static int qmp_write_exact(int fd, const char *str, size_t len, uint32_t timeout_ms) {
    uint64_t deadline = (timeout_ms > 0) ? (qmp_monotonic_ms() + timeout_ms) : 0;
    size_t written = 0;

    while (written < len) {
        int poll_timeout = -1;
        if (deadline > 0) {
            uint64_t now = qmp_monotonic_ms();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            uint64_t left = deadline - now;
            poll_timeout = (left > (uint64_t)INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd pfd = {fd, POLLOUT, 0};
        int pr = poll(&pfd, 1, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        ssize_t n = write(fd, str + written, len - written);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
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
 * @brief Dispatches a QMP command and reads responses until a non-event response is received.
 *
 * @param[in]  client   Connected QMP client.
 * @param[in]  cmd_json JSON command string.
 * @param[out] resp_buf Destination response buffer.
 * @param[in]  resp_cap Buffer capacity.
 * @return 0 on success (containing "return"), or -1 on error.
 */
static int qmp_execute_command(qmp_client_t *client,
                               const char *cmd_json,
                               char *resp_buf,
                               size_t resp_cap) {
    if (client == NULL || client->socket_fd < 0 || cmd_json == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t cmd_len = strlen(cmd_json);
    if (qmp_write_exact(client->socket_fd, cmd_json, cmd_len, client->timeout_ms) != 0) {
        return -1;
    }

    /* Read lines, ignoring asynchronous QMP events ("event": ...) */
    char line[QmpMaxResponseLen];
    while (1) {
        ssize_t n = qmp_read_line(client->socket_fd, line, sizeof(line), client->timeout_ms);
        if (n <= 0) {
            return -1;
        }

        /* Check if line is an asynchronous event notification */
        if (strstr(line, "\"event\"") != NULL) {
            continue; /* Skip async events like SHUTDOWN, RESET, etc. */
        }

        /* Found a command response */
        if (resp_buf != NULL && resp_cap > 0) {
            strncpy(resp_buf, line, resp_cap - 1);
            resp_buf[resp_cap - 1] = '\0';
        }

        if (strstr(line, "\"return\"") != NULL) {
            return 0;
        }
        if (strstr(line, "\"error\"") != NULL) {
            errno = EPROTO;
            return -1;
        }
    }
}

int qmp_connect(qmp_client_t *client, const char *socket_path, uint32_t timeout_ms) {
    if (client == NULL || socket_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    client->timeout_ms = (timeout_ms > 0) ? timeout_ms : QmpDefaultTimeoutMs;
    client->capabilities_negotiated = false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int pr = poll(&pfd, 1, (int)client->timeout_ms);
        if (pr <= 0) {
            close(fd);
            errno = (pr == 0) ? ETIMEDOUT : errno;
            return -1;
        }
        int sock_err = 0;
        socklen_t optlen = sizeof(sock_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &optlen) != 0 || sock_err != 0) {
            close(fd);
            errno = (sock_err != 0) ? sock_err : errno;
            return -1;
        }
    } else if (r < 0) {
        close(fd);
        return -1;
    }

    client->socket_fd = fd;

    /* Read initial greeting banner {"QMP": {"version": ...}} */
    char banner[QmpMaxResponseLen];
    ssize_t n = qmp_read_line(fd, banner, sizeof(banner), client->timeout_ms);
    if (n <= 0 || strstr(banner, "\"QMP\"") == NULL) {
        qmp_close(client);
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int qmp_negotiate_capabilities(qmp_client_t *client) {
    if (client == NULL || client->socket_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    const char *cmd = "{\"execute\": \"qmp_capabilities\"}\r\n";
    if (qmp_execute_command(client, cmd, NULL, 0) != 0) {
        return -1;
    }

    client->capabilities_negotiated = true;
    return 0;
}

int qmp_system_powerdown(qmp_client_t *client) {
    if (client == NULL || client->socket_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    const char *cmd = "{\"execute\": \"system_powerdown\"}\r\n";
    return qmp_execute_command(client, cmd, NULL, 0);
}

int qmp_quit(qmp_client_t *client) {
    if (client == NULL || client->socket_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    const char *cmd = "{\"execute\": \"quit\"}\r\n";
    /* Quit might close socket immediately on return, so accept -1 with ECONNRESET/EPIPE/EOF as ok */
    int r = qmp_execute_command(client, cmd, NULL, 0);
    if (r != 0 && (errno == ECONNRESET || errno == EPIPE)) {
        return 0;
    }
    return r;
}

int qmp_query_status(qmp_client_t *client, int *is_running) {
    if (client == NULL || client->socket_fd < 0 || is_running == NULL) {
        errno = EINVAL;
        return -1;
    }

    char resp[QmpMaxResponseLen];
    const char *cmd = "{\"execute\": \"query-status\"}\r\n";
    if (qmp_execute_command(client, cmd, resp, sizeof(resp)) != 0) {
        return -1;
    }

    if (strstr(resp, "\"status\": \"running\"") != NULL ||
        strstr(resp, "\"running\": true") != NULL) {
        *is_running = 1;
    } else {
        *is_running = 0;
    }

    return 0;
}

void qmp_close(qmp_client_t *client) {
    if (client == NULL) return;
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    client->capabilities_negotiated = false;
}
