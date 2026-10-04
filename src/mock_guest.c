/**
 * @file mock_guest.c
 * @brief Mock guest execution agent daemon for Linux development and testing.
 */

#include "mock_process.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void stop_signal(int sig) {
    (void)sig;
    stopping = 1;
}

static int handshake(decoder_t *rx, int fd) {
    uint64_t end = monotonic_ms() + 10000;
    while (!stopping && monotonic_ms() < end) {
        struct pollfd p = {fd, POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n > 0) {
            int result = wire_read(rx, fd);
            if (result == 1) {
                return (rx->type == WaddleMsgSpawnReq) ? 0 : -1;
            }
            if (result < 0) {
                return -1;
            }
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int flush_response(int fd, queue_t *tx) {
    uint64_t end = monotonic_ms() + 10000;
    while (tx->len > 0 && !stopping && monotonic_ms() < end) {
        struct pollfd p = {fd, POLLOUT, 0};
        int n = poll(&p, 1, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n > 0 && queue_flush(tx, fd) != 0) {
            return -1;
        }
    }
    return (tx->len > 0) ? -1 : 0;
}

static int relay(int fd) {
    decoder_t rx;
    memset(&rx, 0, sizeof(rx));
    queue_t tx;
    memset(&tx, 0, sizeof(tx));
    queue_t input;
    memset(&input, 0, sizeof(input));

    uint32_t seq = 1;
    mock_process_t child;
    memset(&child, 0, sizeof(child));
    child.input = -1;
    child.output[0] = -1;
    child.output[1] = -1;

    int result = 1;
    int input_eof = 0;
    int eof[2] = {0, 0};
    int exit_sent = 0;

    if (queue_init(&tx) != 0 || queue_init(&input) != 0 || handshake(&rx, fd) != 0) {
        goto done;
    }

    if (mock_spawn(&child, rx.body, rx.length) != 0) {
        int error = errno;
        const char *message = strerror(error);
        size_t n = strlen(message);
        uint8_t b[256] = {0};
        waddle_put32(b, (uint32_t)error);
        waddle_put32(b + 8, (uint32_t)n);
        memcpy(b + 12, message, n);
        if (wire_send(&tx, &seq, (uint16_t)WaddleMsgSpawnResp, b, n + 12) == 0) {
            result = flush_response(fd, &tx);
        }
        goto done;
    }

    uint8_t response[12] = {0};
    waddle_put32(response + 4, (uint32_t)child.pid);
    if (wire_send(&tx, &seq, (uint16_t)WaddleMsgSpawnResp, response, 12) != 0) {
        goto done;
    }
    wire_consume(&rx);

    if (child.interactive) {
        if (wire_eof(&tx, &seq, 2) != 0) {
            goto done;
        }
        eof[1] = 1;
    }

    while (!stopping) {
        if (mock_reap(&child) < 0) {
            goto done;
        }

        if (child.reaped && eof[0] && eof[1] && !exit_sent && queue_space(&tx) >= 48) {
            uint8_t b[16] = {0};
            uint32_t code = WIFEXITED(child.status) ? (uint32_t)WEXITSTATUS(child.status)
                                                   : (uint32_t)(128 + WTERMSIG(child.status));
            waddle_put32(b, code);
            waddle_put32(b + 4, WIFSIGNALED(child.status) ? 1 : 0);
            waddle_put64(b + 8, monotonic_ms() - child.started);
            if (wire_send(&tx, &seq, (uint16_t)WaddleMsgProcessExit, b, 16) != 0) {
                goto done;
            }
            exit_sent = 1;
        }

        if (exit_sent && tx.len == 0) {
            result = 0;
            goto done;
        }

        if (input_eof && input.len == 0 && child.input >= 0) {
            close(child.input);
            child.input = -1;
        }

        short fd_events = 0;
        if (!exit_sent && queue_space(&input) >= WaddleChunkSize) {
            fd_events |= POLLIN;
        }
        if (tx.len > 0) {
            fd_events |= POLLOUT;
        }

        struct pollfd p[4] = {
            {fd, fd_events, 0},
            {child.input, (short)(input.len > 0 ? POLLOUT : 0), 0},
            {child.output[0], (short)(queue_space(&tx) >= WaddleChunkSize + 40 ? POLLIN : 0), 0},
            {child.output[1], (short)(queue_space(&tx) >= WaddleChunkSize + 40 ? POLLIN : 0), 0}
        };

        for (int i = 0; i < 4; i++) {
            if (p[i].events == 0) {
                p[i].fd = -1;
            }
        }

        int n = poll(p, 4, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto done;
        }

        if ((p[0].revents & POLLOUT) != 0) {
            if (queue_flush(&tx, fd) != 0) {
                goto done;
            }
        }

        if (p[1].revents != 0) {
            if (queue_flush(&input, child.input) != 0) {
                if (errno != EPIPE && !(child.interactive && errno == EIO)) {
                    goto done;
                }
                close(child.input);
                child.input = -1;
                input.len = 0;
            }
        }

        for (int i = 0; i < 2; i++) {
            if ((p[2 + i].revents != 0) && queue_space(&tx) >= WaddleChunkSize + 40) {
                uint8_t b[WaddleChunkSize];
                ssize_t got = read(child.output[i], b, sizeof(b));
                if (got > 0) {
                    if (wire_stream(&tx, &seq, (unsigned int)i + 1, b, (size_t)got) != 0) {
                        goto done;
                    }
                } else if (got == 0 || (got < 0 && child.interactive && errno == EIO)) {
                    close(child.output[i]);
                    child.output[i] = -1;
                    eof[i] = 1;
                    if (wire_eof(&tx, &seq, (unsigned int)i + 1) != 0) {
                        goto done;
                    }
                } else if (errno != EINTR && errno != EAGAIN) {
                    goto done;
                }
            }
        }

        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            int got = wire_read(&rx, fd);
            if (got == 0) {
                continue;
            }
            if (got < 0) {
                goto done;
            }

            uint8_t *b = rx.body;
            size_t len = rx.length;

            if (rx.type == WaddleMsgStreamData) {
                if (input_eof || len < 8 || b[0] != 0 || b[1] != 0 || b[2] != 0 || b[3] != 0 ||
                    waddle_get32(b + 4) != len - 8 || len - 8 > WaddleChunkSize) {
                    goto protocol;
                }
                if (child.input >= 0 && queue_append(&input, b + 8, len - 8) != 0) {
                    goto done;
                }
            } else if (rx.type == WaddleMsgStreamEof) {
                if (input_eof || len != 4 || waddle_get32(b) != 0) {
                    goto protocol;
                }
                input_eof = 1;
                if (child.interactive && child.input >= 0 && queue_append(&input, "\004", 1) != 0) {
                    goto done;
                }
            } else if (rx.type == WaddleMsgSignalEvent) {
                if (len != 4) {
                    goto protocol;
                }
                uint32_t sig = waddle_get32(b);
                if (sig != SIGINT && sig != SIGQUIT && sig != SIGTERM && sig != SIGKILL) {
                    goto protocol;
                }
                if (!child.reaped && kill(-child.pid, (int)sig) != 0 && errno != ESRCH) {
                    goto done;
                }
            } else if (rx.type == WaddleMsgTerminalResize) {
                if (len != 8 || !child.interactive || waddle_get16(b) == 0 || waddle_get16(b + 2) == 0) {
                    goto protocol;
                }
                struct winsize ws;
                memset(&ws, 0, sizeof(ws));
                ws.ws_row = waddle_get16(b);
                ws.ws_col = waddle_get16(b + 2);
                ws.ws_xpixel = waddle_get16(b + 4);
                ws.ws_ypixel = waddle_get16(b + 6);
                if (!child.reaped && child.output[0] >= 0 && ioctl(child.output[0], TIOCSWINSZ, &ws) != 0) {
                    goto done;
                }
            } else {
                goto protocol;
            }

            wire_consume(&rx);
        }
    }
    goto done;

protocol:
    errno = EPROTO;
done:
    mock_stop(&child);
    wire_destroy(&rx);
    queue_free(&tx);
    queue_free(&input);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "--socket-path") != 0) {
        fprintf(stderr,
                "Usage: waddle-mock-guest --socket-path PATH\n"
                "Linux development peer; serves one session and exits.\n");
        return 2;
    }

    const char *path = argv[2];
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "mock: socket path too long\n");
        return 2;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    struct sigaction a;
    memset(&a, 0, sizeof(a));
    sigemptyset(&a.sa_mask);
    a.sa_handler = stop_signal;
    if (sigaction(SIGTERM, &a, NULL) != 0 || sigaction(SIGINT, &a, NULL) != 0) {
        perror("mock: sigaction");
        return 1;
    }

    a.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &a, NULL) != 0) {
        return 1;
    }

    umask(077);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) {
        perror("mock: socket");
        return 1;
    }

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("mock: bind (existing paths are never removed)");
        close(listener);
        return 1;
    }

    int result = 1;
    int client = -1;

    if (listen(listener, 1) != 0) {
        perror("mock: listen");
        goto done;
    }

    while (!stopping) {
        struct pollfd p = {listener, POLLIN, 0};
        int n = poll(&p, 1, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue;
        }
        client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            break;
        }
        close(listener);
        listener = -1;
        result = relay(client);
        break;
    }

done:
    if (client >= 0) {
        close(client);
    }
    if (listener >= 0) {
        close(listener);
    }
    (void)unlink(path);
    return result;
}
