/**
 * @file session.c
 * @brief Multiplexed stream event loop bridging host I/O and guest transport.
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

/** @brief Fixed receive-drain cap after peer write shutdown; never renewed by traffic. */
#define TransportDrainMs UINT64_C(2000)

int waddle_session(int fd, queue_t *tx, uint32_t seq, uint64_t deadline, int interactive) {
    queue_t output[2];
    memset(&output[0], 0, sizeof(output[0]));
    memset(&output[1], 0, sizeof(output[1]));

    decoder_t rx;
    memset(&rx, 0, sizeof(rx));

    int result = 125;
    int spawned = 0;
    int stdin_eof = 0;
    int tx_error = 0;
    uint64_t drain_deadline = 0;
    int eof[2] = {0, 0};
    int exited = 0;
    uint32_t exit_code = 0;

    if (queue_init(&output[0]) != 0 || queue_init(&output[1]) != 0) {
        goto done;
    }

    for (;;) {
        uint64_t now = monotonic_ms();
        if (deadline > 0 && now >= deadline &&
            (tx_error == 0 || exited || deadline <= drain_deadline)) {
            fprintf(stderr, "waddle: session timed out\n");
            result = 124;
            goto done;
        }

        if (tx_error != 0 && !exited && now >= drain_deadline) {
            errno = tx_error;
            goto error;
        }

        if (spawned && !exited && tx_error == 0 &&
            waddle_send_pending(tx, &seq, interactive) != 0) {
            goto error;
        }

        if (exited && output[0].len == 0 && output[1].len == 0) {
            result = (int)(exit_code & 255);
            goto done;
        }

        short fd_events = 0;
        if (!exited && queue_space(&output[0]) >= WaddleChunkSize &&
            queue_space(&output[1]) >= WaddleChunkSize) {
            fd_events |= POLLIN;
        }
        if (tx->len > 0 && !exited && tx_error == 0) {
            fd_events |= POLLOUT;
        }

        short stdin_events = 0;
        if (spawned && !exited && tx_error == 0 && !stdin_eof && queue_space(tx) >= WaddleChunkSize + 40) {
            stdin_events |= POLLIN;
        }

        struct pollfd p[5] = {
            {fd, fd_events, 0},
            {STDIN_FILENO, stdin_events, 0},
            {STDOUT_FILENO, (short)(output[0].len > 0 ? POLLOUT : 0), 0},
            {STDERR_FILENO, (short)(output[1].len > 0 ? POLLOUT : 0), 0},
            {waddle_signal_fd(), POLLIN, 0}
        };

        /* Disable inactive poll items to avoid perpetual POLLHUP on stdin or sockets */
        if (p[0].events == 0) {
            p[0].fd = -1;
        }
        if (p[1].events == 0) {
            p[1].fd = -1;
        }

        int timeout = -1;
        uint64_t poll_deadline = deadline;
        if (tx_error != 0 && !exited &&
            (poll_deadline == 0 || drain_deadline < poll_deadline)) {
            poll_deadline = drain_deadline;
        }
        if (poll_deadline > 0) {
            uint64_t now = monotonic_ms();
            uint64_t left = (poll_deadline > now) ? (poll_deadline - now) : 0;
            timeout = (left > INT_MAX) ? INT_MAX : (int)left;
        }

        int n = poll(p, 5, timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto error;
        }

        if (p[4].revents != 0) {
            if (spawned && !exited && tx_error == 0) {
                if (waddle_send_pending(tx, &seq, interactive) != 0) {
                    goto error;
                }
            } else {
                waddle_drain_signals();
            }
        }

        for (int i = 0; i < 2; i++) {
            if ((p[2 + i].revents != 0) && queue_flush(&output[i], 1 + i) != 0) {
                goto error;
            }
        }

        if ((p[0].revents & POLLOUT) != 0 && queue_flush(tx, fd) != 0) {
            if (errno != EPIPE && errno != ECONNRESET) {
                goto error;
            }
            /* A peer may close after sending its final frames while our stdin EOF
             * is still queued. Its write failure is not a ProcessExit: stop sending
             * and validate the receive direction before deciding the session result. */
            tx_error = errno;
            drain_deadline = monotonic_ms() + TransportDrainMs;
            tx->off = 0;
            tx->len = 0;
        }

        if (tx_error == 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            uint8_t b[WaddleChunkSize];
            ssize_t got = read(STDIN_FILENO, b, sizeof(b));
            if (got > 0) {
                /* A signal may arrive after poll returned its readiness snapshot. */
                if (waddle_send_pending(tx, &seq, interactive) != 0) {
                    goto error;
                }
                if (wire_stream(tx, &seq, (unsigned int)WaddleStreamStdin, b, (size_t)got) != 0) {
                    goto error;
                }
            } else if (got == 0) {
                if (wire_eof(tx, &seq, (unsigned int)WaddleStreamStdin) != 0) {
                    goto error;
                }
                stdin_eof = 1;
            } else if (errno != EAGAIN && errno != EINTR) {
                goto error;
            }
        }

        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            int got = wire_read(&rx, fd);
            if (got == 0) {
                continue;
            }
            if (got < 0) {
                if (got == -2) {
                    errno = (tx_error != 0) ? tx_error : ECONNRESET;
                }
                goto error;
            }

            uint8_t *b = rx.body;
            size_t len = rx.length;

            if (!spawned) {
                if (rx.type != WaddleMsgSpawnResp || len < 12 || waddle_get32(b + 8) != len - 12) {
                    goto protocol;
                }
                uint32_t status = waddle_get32(b);
                if (status != 0) {
                    fprintf(stderr, "waddle: spawn failed (%u): ", status);
                    (void)fwrite(b + 12, 1, len - 12, stderr);
                    fputc('\n', stderr);
                    result = (status == ENOENT) ? 127 : 126;
                    goto done;
                }
                if (len != 12 || waddle_get32(b + 4) == 0) {
                    goto protocol;
                }
                spawned = 1;
            } else if (rx.type == WaddleMsgStreamData) {
                if (len < 8 || b[0] < 1 || b[0] > 2 || b[1] != 0 || b[2] != 0 || b[3] != 0 ||
                    waddle_get32(b + 4) != len - 8 || len - 8 > WaddleChunkSize ||
                    eof[b[0] - 1]) {
                    goto protocol;
                }
                if (queue_append(&output[b[0] - 1], b + 8, len - 8) != 0) {
                    goto error;
                }
            } else if (rx.type == WaddleMsgStreamEof) {
                if (len != 4 || waddle_get32(b) < 1 || waddle_get32(b) > 2) {
                    goto protocol;
                }
                unsigned int stream = waddle_get32(b) - 1;
                if (eof[stream]) {
                    goto protocol;
                }
                eof[stream] = 1;
            } else if (rx.type == WaddleMsgProcessExit) {
                if (len != 16 || !eof[0] || !eof[1] || waddle_get32(b + 4) > 2) {
                    goto protocol;
                }
                exit_code = waddle_get32(b);
                exited = 1;
                tx->len = 0;
            } else {
                goto protocol;
            }

            wire_consume(&rx);
        }
    }

protocol:
    errno = EPROTO;
error:
    fprintf(stderr, "waddle: session failed: %s\n", strerror(errno));
done:
    wire_destroy(&rx);
    queue_free(&output[0]);
    queue_free(&output[1]);
    return result;
}
