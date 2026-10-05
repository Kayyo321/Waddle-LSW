/**
 * @file daemon_fs.c
 * @brief Implementation of VirtIO-FS lifecycle supervisor and filesystem translation bridge.
 */

#include "daemon_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Returns monotonic time in milliseconds.
 *
 * @return Milliseconds on success, or 0 on system clock failure.
 */
static uint64_t fs_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

/**
 * @brief Appends a duplicated string to the argument vector.
 *
 * @param[in,out] argv Array of strings.
 * @param[in,out] argc Pointer to current argument count.
 * @param[in]     max  Maximum allowed argument capacity.
 * @param[in]     val  String value to duplicate and append.
 * @return 0 on success, or -1 on allocation failure or capacity exceeded.
 */
static int fs_append_arg(char **argv, size_t *argc, size_t max, const char *val) {
    if (*argc + 1 >= max) {
        errno = E2BIG;
        return -1;
    }
    char *copy = strdup(val);
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    argv[(*argc)++] = copy;
    argv[*argc] = NULL;
    return 0;
}

int daemon_fs_find_binary(char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap < 16) {
        errno = EINVAL;
        return -1;
    }

    char exe_dir[WaddleMaxPathLen];
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len > 0) {
        exe_dir[len] = '\0';
        char *last_slash = strrchr(exe_dir, '/');
        if (last_slash != NULL) {
            *last_slash = '\0';
        } else {
            exe_dir[0] = '.';
            exe_dir[1] = '\0';
        }

        /* Check relative vendor paths */
        char candidate[WaddleMaxPathLen + 64];
        snprintf(candidate, sizeof(candidate), "%s/vendor/virtiofsd", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }

        snprintf(candidate, sizeof(candidate), "%s/../build/vendor/virtiofsd", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }

        snprintf(candidate, sizeof(candidate), "%s/virtiofsd", exe_dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidate);
            return 0;
        }
    }

    /* Fallback to local build vendor directory */
    if (access("build/vendor/virtiofsd", X_OK) == 0) {
        snprintf(out_path, path_cap, "build/vendor/virtiofsd");
        return 0;
    }

    /* Standard known install locations */
    const char *candidates[] = {
        "/usr/libexec/virtiofsd",
        "/usr/lib/qemu/virtiofsd",
        "/usr/bin/virtiofsd",
        "/usr/local/bin/virtiofsd",
        NULL
    };

    for (size_t i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out_path, path_cap, "%s", candidates[i]);
            return 0;
        }
    }

    /* Search in PATH */
    const char *path_env = getenv("PATH");
    if (path_env != NULL) {
        char *path_copy = strdup(path_env);
        if (path_copy != NULL) {
            char *token = strtok(path_copy, ":");
            while (token != NULL) {
                char candidate[WaddleMaxPathLen + 64];
                snprintf(candidate, sizeof(candidate), "%s/virtiofsd", token);
                if (access(candidate, X_OK) == 0) {
                    snprintf(out_path, path_cap, "%s", candidate);
                    free(path_copy);
                    return 0;
                }
                token = strtok(NULL, ":");
            }
            free(path_copy);
        }
    }

    errno = ENOENT;
    return -1;
}

int daemon_fs_build_args(const char *socket_path,
                         const char *shared_dir,
                         const char *sandbox_mode,
                         const char *cache_mode,
                         char **argv,
                         size_t max_args) {
    if (socket_path == NULL || shared_dir == NULL || argv == NULL || max_args < 8) {
        errno = EINVAL;
        return -1;
    }

    size_t argc = 0;
    argv[0] = NULL;

    if (fs_append_arg(argv, &argc, max_args, "virtiofsd") != 0) goto fail;

    char sock_arg[WaddleMaxPathLen + 32];
    snprintf(sock_arg, sizeof(sock_arg), "--socket-path=%s", socket_path);
    if (fs_append_arg(argv, &argc, max_args, sock_arg) != 0) goto fail;

    char dir_arg[WaddleMaxPathLen + 32];
    snprintf(dir_arg, sizeof(dir_arg), "--shared-dir=%s", shared_dir);
    if (fs_append_arg(argv, &argc, max_args, dir_arg) != 0) goto fail;

    const char *sandbox = (sandbox_mode != NULL && sandbox_mode[0] != '\0') ? sandbox_mode : "none";
    char sandbox_arg[64];
    snprintf(sandbox_arg, sizeof(sandbox_arg), "--sandbox=%s", sandbox);
    if (fs_append_arg(argv, &argc, max_args, sandbox_arg) != 0) goto fail;

    const char *cache = (cache_mode != NULL && cache_mode[0] != '\0') ? cache_mode : "auto";
    char cache_arg[64];
    snprintf(cache_arg, sizeof(cache_arg), "--cache=%s", cache);
    if (fs_append_arg(argv, &argc, max_args, cache_arg) != 0) goto fail;

    return (int)argc;

fail:
    daemon_fs_free_args(argv, argc);
    return -1;
}

void daemon_fs_free_args(char **argv, size_t argc) {
    if (argv == NULL) return;
    for (size_t i = 0; i < argc; i++) {
        if (argv[i] != NULL) {
            free(argv[i]);
            argv[i] = NULL;
        }
    }
}

/**
 * @brief Probes whether a UNIX domain socket inode has been created and bound by virtiofsd.
 *
 * Checks for socket file existence via stat() without connecting. An active connect()
 * causes virtiofsd (which operates as a 1:1 vhost-user backend) to conclude that the
 * hypervisor has disconnected and immediately shut down.
 *
 * @param[in] path Path to UNIX domain socket.
 * @return 0 if socket inode exists, or -1 otherwise.
 */
static int probe_socket_ready(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        /* Socket file bound; brief sleep ensures listen() is complete */
        usleep(20000);
        return 0;
    }
    return -1;
}

int daemon_fs_spawn(virtiofs_process_t *proc,
                    const char *socket_path,
                    const char *shared_dir,
                    const char *log_path,
                    const char *binary_path,
                    uint32_t timeout_ms) {
    if (proc == NULL || socket_path == NULL || shared_dir == NULL || log_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(proc, 0, sizeof(*proc));
    proc->log_fd = -1;

    /* Verify host directory exists */
    struct stat st;
    if (stat(shared_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = (errno != 0) ? errno : ENOTDIR;
        return -1;
    }

    char resolved_binary[WaddleMaxPathLen];
    const char *bin_to_exec = binary_path;
    if (bin_to_exec == NULL) {
        if (daemon_fs_find_binary(resolved_binary, sizeof(resolved_binary)) != 0) {
            return -1;
        }
        bin_to_exec = resolved_binary;
    }

    /* Remove existing stale socket if present */
    unlink(socket_path);

    char *argv[VirtiofsMaxArgs];
    memset(argv, 0, sizeof(argv));
    int argc = daemon_fs_build_args(socket_path, shared_dir, "none", "auto", argv, VirtiofsMaxArgs);
    if (argc < 0) {
        return -1;
    }

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        daemon_fs_free_args(argv, (size_t)argc);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(log_fd);
        daemon_fs_free_args(argv, (size_t)argc);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        /* Child */
        if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(log_fd);
        execvp(bin_to_exec, argv);
        _exit(127);
    }

    /* Parent */
    daemon_fs_free_args(argv, (size_t)argc);

    proc->pid = pid;
    proc->log_fd = log_fd;
    proc->is_running = 1;
    snprintf(proc->socket_path, sizeof(proc->socket_path), "%s", socket_path);
    snprintf(proc->shared_dir, sizeof(proc->shared_dir), "%s", shared_dir);
    snprintf(proc->log_path, sizeof(proc->log_path), "%s", log_path);

    /* Wait for socket to become ready */
    uint32_t wait_ms = (timeout_ms > 0) ? timeout_ms : VirtiofsDefaultWaitMs;
    uint64_t deadline = fs_monotonic_ms() + wait_ms;

    while (fs_monotonic_ms() < deadline) {
        /* Check if child died prematurely */
        if (daemon_fs_poll_status(proc) == 0) {
            daemon_fs_cleanup(proc);
            errno = ECHILD;
            return -1;
        }

        if (probe_socket_ready(socket_path) == 0) {
            return 0; /* Socket is ready */
        }

        usleep(50000); /* 50ms poll intervals */
    }

    /* Timed out waiting for socket */
    daemon_fs_stop(proc, 1000);
    errno = ETIMEDOUT;
    return -1;
}

int daemon_fs_poll_status(virtiofs_process_t *proc) {
    if (proc == NULL || proc->pid <= 0) {
        if (proc != NULL) proc->is_running = 0;
        return 0;
    }

    int status = 0;
    pid_t r = waitpid(proc->pid, &status, WNOHANG);
    if (r == proc->pid) {
        proc->is_running = 0;
        proc->pid = 0;
        return 0;
    }
    if (r == 0) {
        return 1;
    }
    if (r < 0 && errno == ECHILD) {
        proc->is_running = 0;
        proc->pid = 0;
        return 0;
    }

    return -1;
}

int daemon_fs_stop(virtiofs_process_t *proc, uint32_t timeout_ms) {
    if (proc == NULL || proc->pid <= 0) {
        if (proc != NULL) {
            if (proc->socket_path[0] != '\0') unlink(proc->socket_path);
            proc->is_running = 0;
        }
        return 0;
    }

    /* Send SIGTERM */
    kill(proc->pid, SIGTERM);

    uint64_t deadline = fs_monotonic_ms() + timeout_ms;
    while (fs_monotonic_ms() < deadline) {
        if (daemon_fs_poll_status(proc) == 0) {
            if (proc->socket_path[0] != '\0') unlink(proc->socket_path);
            return 0;
        }
        usleep(50000);
    }

    /* Fallback to SIGKILL */
    kill(proc->pid, SIGKILL);
    waitpid(proc->pid, NULL, 0);
    proc->pid = 0;
    proc->is_running = 0;

    if (proc->socket_path[0] != '\0') {
        unlink(proc->socket_path);
    }

    return 0;
}

int daemon_fs_mounts_to_rules(const waddle_daemon_fs_mount_t *mounts,
                              size_t mount_count,
                              path_rule_t *rules,
                              size_t max_rules) {
    if (mounts == NULL || rules == NULL || max_rules == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t count = (mount_count < max_rules) ? mount_count : max_rules;
    for (size_t i = 0; i < count; i++) {
        rules[i].source = mounts[i].host_path;
        rules[i].target = mounts[i].guest_drive;
    }
    return (int)count;
}

char *daemon_fs_translate_path(const char *path, const daemon_config_t *config) {
    if (path == NULL || config == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (config->mount_count == 0) {
        /* No explicit mounts, use root default rule */
        return waddle_translate_rules(path, NULL, 0);
    }

    path_rule_t rules[WaddleMaxMounts];
    int rule_count = daemon_fs_mounts_to_rules(config->mounts, config->mount_count, rules, WaddleMaxMounts);
    if (rule_count <= 0) {
        return NULL;
    }

    return waddle_translate_rules(path, rules, (size_t)rule_count);
}

void daemon_fs_cleanup(virtiofs_process_t *proc) {
    if (proc == NULL) return;

    if (proc->pid > 0) {
        daemon_fs_stop(proc, 1000);
    }

    if (proc->log_fd >= 0) {
        close(proc->log_fd);
        proc->log_fd = -1;
    }

    if (proc->socket_path[0] != '\0') {
        unlink(proc->socket_path);
    }

    proc->is_running = 0;
}
