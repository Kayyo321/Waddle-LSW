/**
 * @file host.c
 * @brief Main entry point and CLI option parser for the Waddle host execution utility.
 */

#include "session.h"
#include "terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

/**
 * @brief Parses an unsigned 32-bit integer from a string.
 *
 * @param[in]  s   Null-terminated digit string.
 * @param[out] out Destination pointer for parsed value.
 * @return 0 on success, or -1 on parse failure or overflow.
 */
static int number(const char *s, uint32_t *out) {
    if (s == NULL || *s == '\0') {
        return -1;
    }
    uint64_t n = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        n = (n * 10) + (unsigned int)(*s - '0');
        if (n > UINT32_MAX) {
            return -1;
        }
    }
    *out = (uint32_t)n;
    return 0;
}

/**
 * @brief Prints CLI usage instructions to the specified output stream.
 *
 * @param[in,out] f File stream to write usage text into.
 */
static void usage(FILE *f) {
    fprintf(f,
            "Usage: waddle exec|run [options] -- program [arguments...]\n"
            "  --socket-path PATH             UNIX mock transport (default: VSOCK)\n"
            "  --vsock-cid N --vsock-port N   default 3:5242\n"
            "  --pipe|-P                      raw pipe streams (non-interactive)\n"
            "  --interactive|-i|--tty|-t      interactive terminal session (ConPTY)\n"
            "  --cwd PATH                     working directory in guest\n"
            "  --env|-e KEY=VALUE             set environment variable\n"
            "  --translate-path               map Linux paths to guest VirtIO-FS drives\n"
            "  --timeout SECONDS              total session deadline; 0 disables\n");
}

/**
 * @brief Non-blockingly establishes connection to either a UNIX domain mock or a VSOCK peer.
 *
 * @param[in] path     UNIX socket path if mock mode; NULL for VSOCK transport.
 * @param[in] cid      VSOCK Context Identifier (CID).
 * @param[in] port     VSOCK port number.
 * @param[in] deadline Monotonic millisecond deadline for connection completion.
 * @return Connected non-blocking file descriptor on success, or -1 on error.
 */
static int connect_peer(const char *path, uint32_t cid, uint32_t port, uint64_t deadline) {
    int domain = (path != NULL) ? AF_UNIX : AF_VSOCK;
    int fd = socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    int result;
    if (path != NULL) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(a.sun_path)) {
            errno = ENAMETOOLONG;
            close(fd);
            return -1;
        }
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        result = connect(fd, (struct sockaddr *)&a, sizeof(a));
    } else {
        struct sockaddr_vm a;
        memset(&a, 0, sizeof(a));
        a.svm_family = AF_VSOCK;
        a.svm_cid = cid;
        a.svm_port = port;
        result = connect(fd, (struct sockaddr *)&a, sizeof(a));
    }

    if (result == 0) {
        return fd;
    }

    if (errno != EINPROGRESS) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    for (;;) {
        int timeout = -1;
        if (deadline > 0) {
            uint64_t now = monotonic_ms();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                break;
            }
            uint64_t left = deadline - now;
            timeout = (left > INT_MAX) ? INT_MAX : (int)left;
        }

        struct pollfd p = {fd, POLLOUT, 0};
        result = poll(&p, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = ETIMEDOUT;
            }
            break;
        }

        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) != 0) {
            break;
        }
        if (socket_error != 0) {
            errno = socket_error;
            break;
        }
        return fd;
    }

    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

int main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *cwd_arg = NULL;
    uint32_t cid = 3;
    uint32_t port = WaddleDefaultVsockPort;
    uint32_t timeout = 0;
    int interactive = -1;
    int translate = 0;
    int start = 0;
    int result = 2;
    int fd = -1;

    char *cwd = NULL;
    char *command = NULL;
    char **args = NULL;
    queue_t env;
    memset(&env, 0, sizeof(env));
    queue_t tx;
    memset(&tx, 0, sizeof(tx));
    uint8_t *spawn = NULL;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }

    if (argc < 3 || (strcmp(argv[1], "exec") != 0 && strcmp(argv[1], "run") != 0)) {
        usage(stderr);
        return 2;
    }

    if (queue_init(&env) != 0 || queue_init(&tx) != 0) {
        result = 125;
        goto done;
    }

    for (int i = 2; i < argc; i++) {
        const char *opt = argv[i];
        if (strcmp(opt, "--") == 0) {
            start = i + 1;
            break;
        }
        if (strcmp(opt, "--pipe") == 0 || strcmp(opt, "-P") == 0) {
            interactive = 0;
            continue;
        }
        if (strcmp(opt, "--interactive") == 0 || strcmp(opt, "-i") == 0 ||
            strcmp(opt, "--tty") == 0 || strcmp(opt, "-t") == 0) {
            interactive = 1;
            continue;
        }
        if (strcmp(opt, "--translate-path") == 0) {
            translate = 1;
            continue;
        }
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            usage(stdout);
            result = 0;
            goto done;
        }
        if (i + 1 >= argc) {
            goto usage_error;
        }

        const char *val = argv[++i];
        if (strcmp(opt, "--socket-path") == 0) {
            socket_path = val;
        } else if (strcmp(opt, "--cwd") == 0) {
            cwd_arg = val;
        } else if (strcmp(opt, "--vsock-cid") == 0) {
            if (number(val, &cid) != 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--vsock-port") == 0) {
            if (number(val, &port) != 0 || port == 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--timeout") == 0) {
            if (number(val, &timeout) != 0) {
                goto usage_error;
            }
        } else if (strcmp(opt, "--env") == 0 || strcmp(opt, "-e") == 0) {
            const char *equal = strchr(val, '=');
            if (equal == NULL || equal == val ||
                strlen(val) + 1 > WaddleMaxPayloadSize - env.len ||
                queue_append(&env, val, strlen(val) + 1) != 0) {
                goto usage_error;
            }
        } else {
            goto usage_error;
        }
    }

    if (start == 0 || start >= argc || *argv[start] == '\0') {
        goto usage_error;
    }

    if (interactive < 0) {
        interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    }
    if (interactive && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "waddle: interactive mode requires terminal stdin and stdout\n");
        goto done;
    }

    cwd = cwd_arg ? strdup(cwd_arg) : getcwd(NULL, 0);
    if (cwd == NULL) {
        result = 125;
        goto done;
    }

    args = (char **)calloc((size_t)(argc - start) + 1, sizeof(*args));
    if (args == NULL) {
        result = 125;
        goto done;
    }

    for (int i = start; i < argc; i++) {
        args[i - start] = translate ? waddle_translate_path(argv[i]) : strdup(argv[i]);
        if (args[i - start] == NULL) {
            result = 125;
            goto done;
        }
    }

    if (translate) {
        char *mapped = waddle_translate_path(cwd);
        if (mapped == NULL) {
            result = 125;
            goto done;
        }
        free(cwd);
        cwd = mapped;
    }

    command = waddle_quote(args);
    if (command == NULL) {
        result = 125;
        goto done;
    }

    size_t cwd_len = strlen(cwd);
    size_t cmd_len = strlen(command);
    if (cwd_len > WaddleMaxPayloadSize || cmd_len > WaddleMaxPayloadSize ||
        cwd_len + cmd_len + env.len + 26 > WaddleMaxPayloadSize) {
        errno = E2BIG;
        result = 125;
        goto local_error;
    }

    size_t total = cwd_len + cmd_len + env.len + 26;
    spawn = (uint8_t *)calloc(total, 1);
    if (spawn == NULL) {
        result = 125;
        goto done;
    }

    waddle_put32(spawn, interactive ? WaddleSpawnFlagInteractive : WaddleSpawnFlagRawPipes);
    if (translate) {
        waddle_put32(spawn, waddle_get32(spawn) | WaddleSpawnFlagTranslatePath);
    }

    struct winsize ws = {.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    if (interactive && waddle_terminal_size(&ws) != 0) {
        result = 125;
        goto local_error;
    }

    waddle_put16(spawn + 4, ws.ws_row);
    waddle_put16(spawn + 6, ws.ws_col);
    waddle_put16(spawn + 8, ws.ws_xpixel);
    waddle_put16(spawn + 10, ws.ws_ypixel);
    waddle_put32(spawn + 12, (uint32_t)cwd_len);
    waddle_put32(spawn + 16, (uint32_t)cmd_len);
    waddle_put32(spawn + 20, (uint32_t)env.len);

    memcpy(spawn + 24, cwd, cwd_len + 1);
    memcpy(spawn + 25 + cwd_len, command, cmd_len + 1);
    if (env.len > 0) {
        memcpy(spawn + 26 + cwd_len + cmd_len, env.data, env.len);
    }

    if (waddle_terminal_init(interactive) != 0) {
        result = 125;
        goto local_error;
    }

    uint64_t deadline = (timeout > 0) ? (monotonic_ms() + ((uint64_t)timeout * 1000)) : 0;
    fd = connect_peer(socket_path, cid, port, deadline);
    if (fd < 0) {
        result = (errno == ETIMEDOUT) ? 124 : 125;
        goto local_error;
    }

    if (waddle_standard_nonblock() != 0) {
        result = 125;
        goto local_error;
    }

    uint32_t seq = 1;
    if (wire_send(&tx, &seq, (uint16_t)WaddleMsgSpawnReq, spawn, total) != 0) {
        result = 125;
        goto local_error;
    }

    result = waddle_session(fd, &tx, seq, deadline, interactive);
    goto done;

usage_error:
    usage(stderr);
    goto done;

local_error:
    fprintf(stderr, "waddle: %s\n", strerror(errno));

done:
    waddle_terminal_close();
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    free(spawn);
    spawn = NULL;
    free(command);
    command = NULL;
    free(cwd);
    cwd = NULL;
    waddle_free_argv(args);
    args = NULL;
    queue_free(&env);
    queue_free(&tx);
    return result;
}
