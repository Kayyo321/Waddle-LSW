/**
 * @file daemon_client.c
 * @brief Implementation of host CLI client library for Waddle subsystem supervisor daemon.
 */

#include "daemon_client.h"
#include "daemon_config.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int waddle_client_default_runtime_dir(char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        errno = EINVAL;
        return -1;
    }
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != NULL && xdg_runtime[0] != '\0') {
        int n = snprintf(buf, buf_len, "%s/waddle", xdg_runtime);
        if (n < 0 || (size_t)n >= buf_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = "/tmp";
    }
    int n = snprintf(buf, buf_len, "%s/.local/state/waddle/run", home);
    if (n < 0 || (size_t)n >= buf_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int waddle_client_default_socket_path(char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        errno = EINVAL;
        return -1;
    }
    char runtime_dir[WaddleMaxPathLen];
    if (waddle_client_default_runtime_dir(runtime_dir, sizeof(runtime_dir)) != 0) {
        return -1;
    }
    int n = snprintf(buf, buf_len, "%.900s/daemon.sock", runtime_dir);
    if (n < 0 || (size_t)n >= buf_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int waddle_client_default_log_dir(char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        errno = EINVAL;
        return -1;
    }
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = "/tmp";
    }
    int n = snprintf(buf, buf_len, "%s/.local/state/waddle/logs", home);
    if (n < 0 || (size_t)n >= buf_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int waddle_client_connect(const char *socket_path) {
    char resolved[WaddleMaxPathLen];
    const char *path = socket_path;
    if (path == NULL || path[0] == '\0') {
        if (waddle_client_default_socket_path(resolved, sizeof(resolved)) != 0) {
            return -1;
        }
        path = resolved;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

int waddle_client_is_alive(const char *socket_path) {
    int fd = waddle_client_connect(socket_path);
    if (fd < 0) {
        return 0;
    }

    waddle_daemon_status_resp_t status_resp;
    int r = waddle_client_status(fd, &status_resp);
    close(fd);
    return (r == 0) ? 1 : 0;
}

pid_t waddle_client_spawn_daemon(const char *runtime_dir) {
    char daemon_bin[WaddleMaxPathLen];
    const char *env_bin = getenv("WADDLE_DAEMON_BIN");
    if (env_bin != NULL && env_bin[0] != '\0') {
        snprintf(daemon_bin, sizeof(daemon_bin), "%s", env_bin);
    } else {
        char exe_path[WaddleMaxPathLen];
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        int found_relative = 0;
        if (n > 0) {
            exe_path[n] = '\0';
            char *last_slash = strrchr(exe_path, '/');
            if (last_slash != NULL) {
                *last_slash = '\0';
                snprintf(daemon_bin, sizeof(daemon_bin), "%.1000s/waddled", exe_path);
                if (access(daemon_bin, X_OK) == 0) {
                    found_relative = 1;
                }
            }
        }
        if (!found_relative) {
            snprintf(daemon_bin, sizeof(daemon_bin), "waddled");
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child process: redirect standard file descriptors to /dev/null */
        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            if (dev_null > STDERR_FILENO) {
                close(dev_null);
            }
        }

        if (runtime_dir != NULL && runtime_dir[0] != '\0') {
            execlp(daemon_bin, daemon_bin, "--daemonize", "--runtime-dir", runtime_dir, (char *)NULL);
        } else {
            execlp(daemon_bin, daemon_bin, "--daemonize", (char *)NULL);
        }
        _exit(127);
    }

    /* Wait for daemonizing parent to return */
    int status = 0;
    waitpid(pid, &status, 0);
    return pid;
}

int waddle_client_ensure_daemon(const char *socket_path, uint32_t timeout_ms) {
    int fd = waddle_client_connect(socket_path);
    if (fd >= 0) {
        return fd;
    }

    /* Daemon not running; attempt to spawn it */
    char runtime_buf[WaddleMaxPathLen];
    const char *target_runtime = NULL;
    if (socket_path != NULL && socket_path[0] != '\0') {
        snprintf(runtime_buf, sizeof(runtime_buf), "%.1000s", socket_path);
        char *slash = strrchr(runtime_buf, '/');
        if (slash != NULL) {
            *slash = '\0';
            target_runtime = runtime_buf;
        }
    }
    (void)waddle_client_spawn_daemon(target_runtime);

    uint32_t elapsed = 0;
    uint32_t interval_ms = 50;
    while (elapsed < timeout_ms) {
        struct timespec ts = {0, (long)(interval_ms * 1000000L)};
        nanosleep(&ts, NULL);
        elapsed += interval_ms;

        fd = waddle_client_connect(socket_path);
        if (fd >= 0) {
            return fd;
        }
    }

    errno = ETIMEDOUT;
    return -1;
}

int waddle_client_start(int fd,
                        uint32_t flags,
                        uint32_t timeout_sec,
                        waddle_daemon_result_resp_t *resp) {
    if (fd < 0 || resp == NULL) {
        errno = EINVAL;
        return -1;
    }

    waddle_daemon_start_req_t req;
    memset(&req, 0, sizeof(req));
    req.flags = flags;
    req.timeout_sec = timeout_sec;

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgStartReq, current_seq, &req, sizeof(req)) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    /* Allow extended timeout if waiting for guest */
    uint32_t wait_ms = (timeout_sec > 0) ? (timeout_sec * 1000 + 5000) : 65000;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, resp, sizeof(*resp), wait_ms) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgStartResp && resp_hdr.msg_type != DaemonMsgErrorResp) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int waddle_client_stop(int fd,
                       uint32_t force,
                       uint32_t timeout_sec,
                       waddle_daemon_result_resp_t *resp) {
    if (fd < 0 || resp == NULL) {
        errno = EINVAL;
        return -1;
    }

    waddle_daemon_stop_req_t req;
    memset(&req, 0, sizeof(req));
    req.force = force;
    req.timeout_sec = timeout_sec;

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgStopReq, current_seq, &req, sizeof(req)) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    uint32_t wait_ms = (timeout_sec > 0) ? (timeout_sec * 1000 + 5000) : 25000;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, resp, sizeof(*resp), wait_ms) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgStopResp && resp_hdr.msg_type != DaemonMsgErrorResp) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int waddle_client_shutdown(int fd, waddle_daemon_result_resp_t *resp) {
    if (fd < 0 || resp == NULL) { errno = EINVAL; return -1; }
    const uint32_t sequence = UINT32_C(0x53485554);
    if (waddle_daemon_send_msg(fd, DaemonMsgShutdownReq, sequence, NULL, 0) != 0) return -1;
    waddle_daemon_header_t header;
    memset(resp, 0, sizeof(*resp));
    if (waddle_daemon_recv_msg(fd, &header, resp, sizeof(*resp), WaddleClientDefaultTimeoutMs) != 0) return -1;
    if ((header.msg_type != DaemonMsgShutdownResp && header.msg_type != DaemonMsgErrorResp) ||
        header.sequence != sequence || header.payload_len != sizeof(*resp)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/** @brief Wait for socket removal and the stable OFD lease to become available.
 * @param[in] socket_path Borrowed explicit socket path or NULL for legacy transport.
 * @param[in] timeout_sec Deadline seconds (0 means 15); bounded by callers.
 * @return 0 fully exited, -1 with errno; closes all temporary descriptors.
 * @note No lock inode is ever removed; lease lock is released on fd close.
 */
int waddle_client_wait_supervisor_exit(const char *socket_path, uint32_t timeout_sec) {
    char socket_buffer[WaddleMaxPathLen];
    if (socket_path == NULL) {
        if (waddle_client_default_socket_path(socket_buffer, sizeof(socket_buffer)) != 0) return -1;
        socket_path = socket_buffer;
    }
    char lease_path[WaddleMaxPathLen];
    size_t len = strlen(socket_path);
    if (len >= sizeof(lease_path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(lease_path, socket_path, len + 1);
    char *slash = strrchr(lease_path, '/');
    if (slash == NULL || (size_t)(slash - lease_path) + sizeof("/waddle.lock") > sizeof(lease_path)) {
        errno = EINVAL; return -1;
    }
    memcpy(slash, "/waddle.lock", sizeof("/waddle.lock"));
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    uint64_t deadline = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000 +
                        (uint64_t)(timeout_sec ? timeout_sec : 15) * 1000;
    for (;;) {
        struct stat st;
        if (lstat(socket_path, &st) != 0 && errno == ENOENT) {
            int fd = open(lease_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 && errno == ENOENT) return 0;
            if (fd < 0) return -1;
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
                close(fd); errno = EACCES; return -1;
            }
            struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
            int result = fcntl(fd, F_OFD_SETLK, &lock);
            int saved = errno;
            close(fd);
            if (result == 0) return 0;
            if (saved != EACCES && saved != EAGAIN) { errno = saved; return -1; }
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        uint64_t current = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
        if (current >= deadline) { errno = ETIMEDOUT; return -1; }
        struct timespec delay = {0, 10000000L};
        if (nanosleep(&delay, NULL) != 0 && errno == EINTR) return -1;
    }
}

int waddle_client_status(int fd, waddle_daemon_status_resp_t *resp) {
    if (fd < 0 || resp == NULL) {
        errno = EINVAL;
        return -1;
    }

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgStatusReq, current_seq, NULL, 0) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, resp, sizeof(*resp), WaddleClientDefaultTimeoutMs) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgStatusResp) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int waddle_client_kill(int fd, waddle_daemon_result_resp_t *resp) {
    if (fd < 0 || resp == NULL) {
        errno = EINVAL;
        return -1;
    }

    waddle_daemon_kill_req_t req;
    memset(&req, 0, sizeof(req));

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgKillReq, current_seq, &req, sizeof(req)) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, resp, sizeof(*resp), WaddleClientDefaultTimeoutMs) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgKillResp && resp_hdr.msg_type != DaemonMsgErrorResp) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int waddle_client_fs_list(int fd, waddle_daemon_fs_list_resp_t *resp) {
    if (fd < 0 || resp == NULL) {
        errno = EINVAL;
        return -1;
    }

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgFsListReq, current_seq, NULL, 0) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, resp, sizeof(*resp), WaddleClientDefaultTimeoutMs) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgFsListResp) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int waddle_client_logs(int fd,
                       uint32_t target,
                       uint32_t lines,
                       char *buf,
                       size_t buf_len,
                       size_t *out_len) {
    if (fd < 0 || buf == NULL || buf_len == 0) {
        errno = EINVAL;
        return -1;
    }

    waddle_daemon_logs_req_t req;
    memset(&req, 0, sizeof(req));
    req.target = target;
    req.lines = lines;

    static uint32_t seq = 1;
    uint32_t current_seq = seq++;

    if (waddle_daemon_send_msg(fd, DaemonMsgLogsReq, current_seq, &req, sizeof(req)) != 0) {
        return -1;
    }

    waddle_daemon_header_t resp_hdr;
    if (waddle_daemon_recv_msg(fd, &resp_hdr, buf, (uint32_t)(buf_len - 1), WaddleClientDefaultTimeoutMs) != 0) {
        return -1;
    }

    if (resp_hdr.msg_type != DaemonMsgLogsResp) {
        errno = EPROTO;
        return -1;
    }

    buf[resp_hdr.payload_len] = '\0';
    if (out_len != NULL) {
        *out_len = (size_t)resp_hdr.payload_len;
    }

    return 0;
}

int waddle_client_cmd_start(const char *socket_path, int wait_guest, uint32_t timeout_sec) {
    printf("[waddle] Starting background subsystem...\n");
    int fd = waddle_client_ensure_daemon(socket_path, WaddleDaemonSpawnTimeoutMs);
    if (fd < 0) {
        fprintf(stderr, "waddle: failed to connect to daemon supervisor: %s\n", strerror(errno));
        return 1;
    }

    uint32_t flags = wait_guest ? DaemonStartFlagWaitGuest : 0;
    waddle_daemon_result_resp_t resp;
    memset(&resp, 0, sizeof(resp));

    if (waddle_client_start(fd, flags, timeout_sec, &resp) != 0) {
        fprintf(stderr, "waddle: daemon communication error: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    if (resp.status_code != 0) {
        fprintf(stderr, "waddle: failed to start subsystem: %s (code %u)\n",
                resp.error_msg[0] ? resp.error_msg : strerror((int)resp.status_code),
                (unsigned)resp.status_code);
        return 1;
    }

    printf("[waddle] Subsystem started successfully (%s).\n",
           waddle_subsystem_state_to_string((waddle_subsystem_state_t)resp.subsystem_state));
    return 0;
}

int waddle_client_cmd_stop(const char *socket_path, int force, uint32_t timeout_sec) {
    int fd = waddle_client_connect(socket_path);
    if (fd < 0) {
        if (waddle_client_wait_supervisor_exit(socket_path, timeout_sec) != 0) {
            fprintf(stderr, "waddle: supervisor is unreachable or still owns its lease: %s\n", strerror(errno));
            return 1;
        }
        printf("[waddle] Subsystem daemon is not running.\n");
        return 0;
    }

    printf("[waddle] Stopping background subsystem (%s)...\n",
           force ? "force SIGKILL" : "graceful ACPI");

    waddle_daemon_result_resp_t resp;
    memset(&resp, 0, sizeof(resp));

    if (waddle_client_stop(fd, (uint32_t)force, timeout_sec, &resp) != 0) {
        fprintf(stderr, "waddle: daemon communication error: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (resp.status_code != 0) {
        close(fd);
        fprintf(stderr, "waddle: stop failed: %s (code %u)\n",
                resp.error_msg[0] ? resp.error_msg : strerror((int)resp.status_code),
                (unsigned)resp.status_code);
        return 1;
    }

    if (waddle_client_shutdown(fd, &resp) != 0 || resp.status_code != 0) {
        int saved = resp.status_code ? (int)resp.status_code : errno;
        close(fd);
        fprintf(stderr, "waddle: supervisor shutdown failed: %s\n", strerror(saved));
        return 1;
    }
    close(fd);
    if (waddle_client_wait_supervisor_exit(socket_path, timeout_sec) != 0) {
        fprintf(stderr, "waddle: supervisor exit failed: %s\n", strerror(errno));
        return 1;
    }
    printf("[waddle] Subsystem stopped successfully.\n");
    return 0;
}

int waddle_client_cmd_restart(const char *socket_path, int force, uint32_t timeout_sec) {
    printf("[waddle] Restarting subsystem...\n");
    int stopped = waddle_client_cmd_stop(socket_path, force, timeout_sec);
    if (stopped != 0) return stopped;
    return waddle_client_cmd_start(socket_path, 1, timeout_sec);
}

int waddle_client_cmd_status(const char *socket_path, int json_output) {
    int fd = waddle_client_connect(socket_path);
    if (fd < 0) {
        if (json_output) {
            printf("{\n"
                   "  \"state\": \"stopped\",\n"
                   "  \"state_code\": 0,\n"
                   "  \"daemon_pid\": 0,\n"
                   "  \"qemu_pid\": 0,\n"
                   "  \"virtiofsd_pid\": 0,\n"
                   "  \"vsock_cid\": %u,\n"
                   "  \"vsock_port\": %u,\n"
                   "  \"uptime_sec\": 0,\n"
                   "  \"memory_mb\": 0,\n"
                   "  \"vcpus\": 0,\n"
                   "  \"mount_count\": 0,\n"
                   "  \"mounts\": []\n"
                   "}\n",
                   (unsigned)WaddleDefaultVsockCid,
                   (unsigned)WaddleDefaultVsockPortVal);
        } else {
            printf("Waddle Subsystem Status:\n");
            printf("  State:         Stopped\n");
            printf("  Daemon PID:    -\n");
            printf("  QEMU PID:      -\n");
            printf("  VirtIO-FS PID: -\n");
        }
        return 0;
    }

    waddle_daemon_status_resp_t status;
    memset(&status, 0, sizeof(status));

    if (waddle_client_status(fd, &status) != 0) {
        fprintf(stderr, "waddle: failed to retrieve status from daemon: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    const char *state_str = waddle_subsystem_state_to_string((waddle_subsystem_state_t)status.subsystem_state);

    if (json_output) {
        printf("{\n");
        printf("  \"state\": \"%s\",\n", state_str);
        printf("  \"state_code\": %u,\n", (unsigned)status.subsystem_state);
        printf("  \"daemon_pid\": %u,\n", (unsigned)status.daemon_pid);
        printf("  \"qemu_pid\": %u,\n", (unsigned)status.qemu_pid);
        printf("  \"virtiofsd_pid\": %u,\n", (unsigned)status.virtiofsd_pid);
        printf("  \"vsock_cid\": %u,\n", (unsigned)status.vsock_cid);
        printf("  \"vsock_port\": %u,\n", (unsigned)status.vsock_port);
        printf("  \"uptime_sec\": %llu,\n", (unsigned long long)status.uptime_sec);
        printf("  \"memory_mb\": %u,\n", (unsigned)status.memory_mb);
        printf("  \"vcpus\": %u,\n", (unsigned)status.vcpus);
        printf("  \"mount_count\": %u,\n", (unsigned)status.mount_count);
        printf("  \"mounts\": [\n");
        for (uint32_t i = 0; i < status.mount_count; i++) {
            printf("    {\n");
            printf("      \"host_path\": \"%s\",\n", status.mounts[i].host_path);
            printf("      \"guest_drive\": \"%s\",\n", status.mounts[i].guest_drive);
            printf("      \"read_only\": %s\n", status.mounts[i].read_only ? "true" : "false");
            printf("    }%s\n", (i + 1 < status.mount_count) ? "," : "");
        }
        printf("  ]\n");
        printf("}\n");
    } else {
        printf("Waddle Subsystem Status:\n");
        printf("  State:         %s\n", state_str);
        if (status.daemon_pid > 0) {
            printf("  Daemon PID:    %u\n", (unsigned)status.daemon_pid);
        } else {
            printf("  Daemon PID:    -\n");
        }
        if (status.qemu_pid > 0) {
            printf("  QEMU PID:      %u\n", (unsigned)status.qemu_pid);
        } else {
            printf("  QEMU PID:      -\n");
        }
        if (status.virtiofsd_pid > 0) {
            printf("  VirtIO-FS PID: %u\n", (unsigned)status.virtiofsd_pid);
        } else {
            printf("  VirtIO-FS PID: -\n");
        }
        printf("  VSOCK:         CID %u, Port %u\n", (unsigned)status.vsock_cid, (unsigned)status.vsock_port);
        printf("  Uptime:        %llus\n", (unsigned long long)status.uptime_sec);
        printf("  Memory:        %u MB\n", (unsigned)status.memory_mb);
        printf("  vCPUs:         %u\n", (unsigned)status.vcpus);
        printf("Active Filesystem Mounts (%u):\n", (unsigned)status.mount_count);
        for (uint32_t i = 0; i < status.mount_count; i++) {
            printf("  - Host: %s -> Guest: %s (%s)\n",
                   status.mounts[i].host_path,
                   status.mounts[i].guest_drive,
                   status.mounts[i].read_only ? "ro" : "rw");
        }
    }

    return 0;
}

int waddle_client_cmd_kill(const char *socket_path) {
    int fd = waddle_client_connect(socket_path);
    if (fd < 0) {
        return waddle_client_wait_supervisor_exit(socket_path, 1) == 0 ? 0 : 1;
    }
    waddle_daemon_result_resp_t resp = {0};
    if (waddle_client_kill(fd, &resp) != 0 || resp.status_code != 0 ||
        waddle_client_shutdown(fd, &resp) != 0 || resp.status_code != 0) {
        int saved = resp.status_code ? (int)resp.status_code : errno;
        close(fd);
        fprintf(stderr, "waddle: kill/shutdown failed: %s\n", strerror(saved));
        return 1;
    }
    close(fd);
    if (waddle_client_wait_supervisor_exit(socket_path, 15) != 0) return 1;
    printf("[waddle] Subsystem children reaped and supervisor exited.\n");
    return 0;
}

int waddle_client_cmd_fs(const char *socket_path) {
    int fd = waddle_client_connect(socket_path);
    if (fd >= 0) {
        waddle_daemon_fs_list_resp_t fs_list;
        memset(&fs_list, 0, sizeof(fs_list));
        if (waddle_client_fs_list(fd, &fs_list) == 0) {
            close(fd);
            printf("Active VirtIO-FS Exports (%u):\n", (unsigned)fs_list.mount_count);
            for (uint32_t i = 0; i < fs_list.mount_count; i++) {
                printf("  [%u] %s -> %s (%s)\n",
                       (unsigned)(i + 1),
                       fs_list.mounts[i].host_path,
                       fs_list.mounts[i].guest_drive,
                       fs_list.mounts[i].read_only ? "read-only" : "read-write");
            }
            return 0;
        }
        close(fd);
    }

    /* Fallback: read configured mounts directly from config.ini */
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    (void)daemon_config_load_file(&config, NULL);

    printf("Configured VirtIO-FS Exports (%u):\n", (unsigned)config.mount_count);
    for (size_t i = 0; i < config.mount_count; i++) {
        printf("  [%u] %s -> %s (%s)\n",
               (unsigned)(i + 1),
               config.mounts[i].host_path,
               config.mounts[i].guest_drive,
               config.mounts[i].read_only ? "read-only" : "read-write");
    }

    return 0;
}

int waddle_client_cmd_logs(const char *socket_path, int follow, uint32_t lines) {
    char log_dir[WaddleMaxPathLen];
    int found_custom_dir = 0;
    if (socket_path != NULL && socket_path[0] != '\0') {
        char runtime_dir[WaddleMaxPathLen];
        snprintf(runtime_dir, sizeof(runtime_dir), "%.1000s", socket_path);
        char *slash = strrchr(runtime_dir, '/');
        if (slash != NULL) {
            *slash = '\0';
            snprintf(log_dir, sizeof(log_dir), "%.900s/logs", runtime_dir);
            struct stat st;
            if (stat(log_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                found_custom_dir = 1;
            }
        }
    }
    if (!found_custom_dir) {
        if (waddle_client_default_log_dir(log_dir, sizeof(log_dir)) != 0) {
            fprintf(stderr, "waddle: failed to resolve log directory\n");
            return 1;
        }
    }

    char daemon_log[WaddleMaxPathLen];
    snprintf(daemon_log, sizeof(daemon_log), "%.900s/daemon.log", log_dir);

    int log_fd = open(daemon_log, O_RDONLY);
    if (log_fd < 0) {
        snprintf(daemon_log, sizeof(daemon_log), "%.900s/qemu.log", log_dir);
        log_fd = open(daemon_log, O_RDONLY);
    }
    if (log_fd < 0) {
        /* Try querying daemon directly via IPC */
        int fd = waddle_client_connect(socket_path);
        if (fd >= 0) {
            char buf[8192];
            size_t out_len = 0;
            if (waddle_client_logs(fd, 0, lines, buf, sizeof(buf), &out_len) == 0 && out_len > 0) {
                printf("%s\n", buf);
                close(fd);
                return 0;
            }
            close(fd);
        }
        printf("[waddle] No log files found in %s\n", log_dir);
        return 0;
    }

    /* Read tail of log file */
    char buf[4096];
    off_t sz = lseek(log_fd, 0, SEEK_END);
    off_t start = (sz > (off_t)sizeof(buf)) ? (sz - (off_t)sizeof(buf)) : 0;
    lseek(log_fd, start, SEEK_SET);

    ssize_t n;
    while ((n = read(log_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }
    fflush(stdout);

    if (follow) {
        for (;;) {
            struct timespec ts = {0, 200000000L};
            nanosleep(&ts, NULL);
            while ((n = read(log_fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                fputs(buf, stdout);
                fflush(stdout);
            }
        }
    }

    close(log_fd);
    return 0;
}
