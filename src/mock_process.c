/**
 * @file mock_process.c
 * @brief Subprocess lifecycle management for Linux mock guest agent.
 */

#include "mock_process.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_error(int fd, int error) {
    ssize_t n;
    do {
        n = write(fd, &error, sizeof(error));
    } while (n < 0 && errno == EINTR);
    _exit(126);
}

static void close_pair(int p[2]) {
    for (int i = 0; i < 2; i++) {
        if (p[i] >= 0) {
            close(p[i]);
            p[i] = -1;
        }
    }
}

int mock_spawn(mock_process_t *p, uint8_t *body, size_t length) {
    if (p == NULL || body == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->input = -1;
    p->output[0] = -1;
    p->output[1] = -1;

    if (length < 26) {
        errno = EPROTO;
        return -1;
    }

    uint32_t flags = waddle_get32(body);
    uint32_t cwd_len = waddle_get32(body + 12);
    uint32_t cmd_len = waddle_get32(body + 16);
    uint32_t env_len = waddle_get32(body + 20);

    if ((uint64_t)cwd_len + cmd_len + env_len + 26 != length || cmd_len == 0 ||
        (flags & ~(WaddleSpawnFlagInteractive | WaddleSpawnFlagRawPipes | WaddleSpawnFlagTranslatePath)) != 0 ||
        ((flags & 3) != 1 && (flags & 3) != 2)) {
        errno = EPROTO;
        return -1;
    }

    char *cwd = (char *)body + 24;
    char *cmd = cwd + cwd_len + 1;
    char *env = cmd + cmd_len + 1;

    if (cwd[cwd_len] != '\0' || cmd[cmd_len] != '\0' ||
        memchr(cwd, 0, cwd_len) != NULL || memchr(cmd, 0, cmd_len) != NULL) {
        errno = EPROTO;
        return -1;
    }

    for (size_t off = 0; off < env_len;) {
        char *end = (char *)memchr(env + off, 0, env_len - off);
        if (end == NULL) {
            errno = EPROTO;
            return -1;
        }
        char *equal = (char *)memchr(env + off, '=', (size_t)(end - env - off));
        if (equal == NULL || equal == env + off) {
            errno = EPROTO;
            return -1;
        }
        off = (size_t)(end - env) + 1;
    }

    char **argv = waddle_unquote(cmd);
    if (argv == NULL) {
        return -1;
    }
    if (argv[0][0] == '\0') {
        waddle_free_argv(argv);
        errno = EPROTO;
        return -1;
    }

    p->interactive = ((flags & WaddleSpawnFlagInteractive) != 0);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = waddle_get16(body + 4);
    ws.ws_col = waddle_get16(body + 6);
    ws.ws_xpixel = waddle_get16(body + 8);
    ws.ws_ypixel = waddle_get16(body + 10);

    if (p->interactive && (ws.ws_row == 0 || ws.ws_col == 0)) {
        waddle_free_argv(argv);
        errno = EPROTO;
        return -1;
    }

    int error_pipe[2] = {-1, -1};
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    int master = -1;

    if (pipe2(error_pipe, O_CLOEXEC) != 0) {
        goto failure;
    }
    if (!p->interactive && (pipe2(in, O_CLOEXEC) != 0 ||
                            pipe2(out, O_CLOEXEC) != 0 ||
                            pipe2(err, O_CLOEXEC) != 0)) {
        goto failure;
    }

    p->started = monotonic_ms();
    p->pid = p->interactive ? forkpty(&master, NULL, NULL, &ws) : fork();
    if (p->pid < 0) {
        p->pid = 0;
        goto failure;
    }

    if (p->pid == 0) {
        close(error_pipe[0]);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        if (!p->interactive) {
            if (setsid() < 0 || dup2(in[0], STDIN_FILENO) < 0 ||
                dup2(out[1], STDOUT_FILENO) < 0 || dup2(err[1], STDERR_FILENO) < 0) {
                child_error(error_pipe[1], errno);
            }
            close_pair(in);
            close_pair(out);
            close_pair(err);
        }

        if (chdir(cwd) != 0) {
            child_error(error_pipe[1], errno);
        }

        for (size_t off = 0; off < env_len;) {
            char *item = env + off;
            size_t n = strlen(item);
            char *equal = strchr(item, '=');
            if (equal != NULL) {
                *equal = '\0';
                if (setenv(item, equal + 1, 1) != 0) {
                    child_error(error_pipe[1], errno);
                }
            }
            off += n + 1;
        }

        execvp(argv[0], argv);
        child_error(error_pipe[1], errno);
    }

    close(error_pipe[1]);
    error_pipe[1] = -1;

    if (p->interactive) {
        if (fcntl(master, F_SETFD, FD_CLOEXEC) != 0) {
            goto failure;
        }
        p->input = dup(master);
        p->output[0] = master;
        master = -1;
        if (p->input < 0 || fcntl(p->input, F_SETFD, FD_CLOEXEC) != 0) {
            goto failure;
        }
    } else {
        close(in[0]);
        in[0] = -1;
        close(out[1]);
        out[1] = -1;
        close(err[1]);
        err[1] = -1;

        p->input = in[1];
        in[1] = -1;
        p->output[0] = out[0];
        out[0] = -1;
        p->output[1] = err[0];
        err[0] = -1;
    }

    int error = 0;
    size_t have = 0;
    for (;;) {
        ssize_t n = read(error_pipe[0], (uint8_t *)&error + have, sizeof(error) - have);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            goto failure;
        }
        if (n == 0) {
            break;
        }
        have += (size_t)n;
        if (have == sizeof(error)) {
            break;
        }
    }
    close(error_pipe[0]);
    error_pipe[0] = -1;

    if (have > 0) {
        errno = (have == sizeof(error)) ? error : EIO;
        goto failure;
    }

    if (nonblock(p->input) != 0 || nonblock(p->output[0]) != 0 ||
        (p->output[1] >= 0 && nonblock(p->output[1]) != 0)) {
        goto failure;
    }

    waddle_free_argv(argv);
    return 0;

failure: {
    int saved_errno = errno;
    close_pair(error_pipe);
    close_pair(in);
    close_pair(out);
    close_pair(err);
    if (master >= 0) {
        close(master);
    }
    mock_stop(p);
    waddle_free_argv(argv);
    errno = saved_errno;
    return -1;
}
}

int mock_reap(mock_process_t *p) {
    if (p == NULL) {
        return -1;
    }
    if (p->reaped) {
        return 1;
    }
    pid_t result = waitpid(p->pid, &p->status, WNOHANG);
    if (result < 0) {
        return -1;
    }
    if (result == p->pid) {
        p->reaped = 1;
        (void)kill(-p->pid, SIGKILL);
        return 1;
    }
    return 0;
}

void mock_stop(mock_process_t *p) {
    if (p == NULL) {
        return;
    }
    if (p->pid > 0) {
        (void)kill(-p->pid, SIGKILL);
        if (!p->reaped) {
            (void)kill(p->pid, SIGKILL);
            while (waitpid(p->pid, &p->status, 0) < 0 && errno == EINTR) {
                /* Wait until fully reaped */
            }
            p->reaped = 1;
        }
    }
    if (p->input >= 0) {
        close(p->input);
        p->input = -1;
    }
    for (int i = 0; i < 2; i++) {
        if (p->output[i] >= 0) {
            close(p->output[i]);
            p->output[i] = -1;
        }
    }
}
