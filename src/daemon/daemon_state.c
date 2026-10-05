/**
 * @file daemon_state.c
 * @brief Implementation of subsystem operational state machine and process orchestration.
 */

#include "daemon_state.h"
#include "daemon_device.h"
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
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

static uint64_t state_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Recursively ensures that a directory path exists with permissions 0700.
 *
 * @param[in] path NUL-terminated absolute directory path.
 * @return 0 on success, or -1 on failure.
 */
static int ensure_dir(const char *path) {
    int fd = daemon_device_open_directory(path, 1);
    if (fd < 0) return -1;
    struct stat st;
    int result = fstat(fd, &st);
    if (result == 0 && (st.st_uid != getuid() || (st.st_mode & 077) != 0)) { errno = EACCES; result = -1; }
    int saved = errno; close(fd); errno = saved;
    return result;
}

int daemon_state_init(daemon_state_t *s, const char *custom_runtime_dir) {
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->state = SubsystemStateStopped;
    s->lock_fd = -1;
    s->qemu.pid = 0;
    s->qemu.log_fd = -1;
    s->qemu.qmp.socket_fd = -1;
    s->virtiofs.pid = 0;
    s->virtiofs.log_fd = -1;

    /* Initialize configuration */
    daemon_config_init_defaults(&s->config);

    int config_loaded = 0;
    if (custom_runtime_dir != NULL && custom_runtime_dir[0] != '\0') {
        const char *last_slash = strrchr(custom_runtime_dir, '/');
        const char *dev_name = (last_slash != NULL) ? (last_slash + 1) : custom_runtime_dir;
        if (dev_name != NULL && dev_name[0] != '\0' && strcmp(dev_name, "waddle") != 0 && strcmp(dev_name, "run") != 0) {
            device_info_t dev_info;
            if (daemon_device_find(dev_name, &dev_info) == 0) {
                if (daemon_config_load_file(&s->config, dev_info.config_path) == 0) {
                    config_loaded = 1;
                }
            }
        }
    }
    if (!config_loaded) {
        device_list_t dev_list;
        if (daemon_device_list(&dev_list) == 1) {
            (void)daemon_config_load_file(&s->config, dev_list.devices[0].config_path);
        } else {
            (void)daemon_config_load_file(&s->config, NULL);
        }
    }

    /* Determine runtime directory */
    if (custom_runtime_dir != NULL && custom_runtime_dir[0] != '\0') {
        snprintf(s->runtime_dir, sizeof(s->runtime_dir), "%s", custom_runtime_dir);
    } else {
        const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
        if (xdg_runtime != NULL && xdg_runtime[0] != '\0') {
            snprintf(s->runtime_dir, sizeof(s->runtime_dir), "%s/waddle", xdg_runtime);
        } else {
            const char *home = getenv("HOME");
            if (home == NULL) home = "/tmp";
            snprintf(s->runtime_dir, sizeof(s->runtime_dir), "%s/.local/state/waddle/run", home);
        }
    }

    if (ensure_dir(s->runtime_dir) != 0) {
        return -1;
    }

    if (strlen(s->runtime_dir) + 32 >= WaddleMaxPathLen) {
        errno = ENAMETOOLONG;
        return -1;
    }

    snprintf(s->lock_path, sizeof(s->lock_path), "%.900s/waddle.lock", s->runtime_dir);
    snprintf(s->daemon_sock_path, sizeof(s->daemon_sock_path), "%.900s/daemon.sock", s->runtime_dir);
    snprintf(s->qmp_sock_path, sizeof(s->qmp_sock_path), "%.900s/qmp.sock", s->runtime_dir);
    snprintf(s->virtiofsd_sock_path, sizeof(s->virtiofsd_sock_path), "%.900s/virtiofsd.sock", s->runtime_dir);

    const char *mock_sock = getenv("WADDLE_MOCK_GUEST_SOCK");
    if (mock_sock != NULL) {
        snprintf(s->mock_guest_sock_path, sizeof(s->mock_guest_sock_path), "%.1000s", mock_sock);
    }

    char named_socket[WaddleMaxPathLen];
    const char *runtime_name = strrchr(s->runtime_dir, '/');
    int named_runtime = config_loaded && runtime_name != NULL &&
        daemon_device_get_socket_path(runtime_name + 1, named_socket, sizeof(named_socket)) == 0 &&
        strcmp(named_socket, s->daemon_sock_path) == 0;

    /* Determine log directory */
    if (custom_runtime_dir != NULL && custom_runtime_dir[0] != '\0') {
        const char *device_name = strrchr(custom_runtime_dir, '/');
        device_info_t device;
        if (named_runtime && device_name != NULL && daemon_device_find(device_name + 1, &device) == 0 && device.config_valid) {
            int written = snprintf(s->log_dir, sizeof(s->log_dir), "%s/logs", device.state_dir);
            if (written < 0 || (size_t)written >= sizeof(s->log_dir)) { errno = ENAMETOOLONG; return -1; }
        } else {
            snprintf(s->log_dir, sizeof(s->log_dir), "%.900s/logs", s->runtime_dir);
        }
    } else {
        const char *home = getenv("HOME");
        if (home == NULL) home = "/tmp";
        snprintf(s->log_dir, sizeof(s->log_dir), "%.900s/.local/state/waddle/logs", home);
    }

    if (!named_runtime && ensure_dir(s->log_dir) != 0) {
        return -1;
    }

    if (strlen(s->log_dir) + 32 >= WaddleMaxPathLen) {
        errno = ENAMETOOLONG;
        return -1;
    }

    snprintf(s->daemon_log_path, sizeof(s->daemon_log_path), "%.900s/daemon.log", s->log_dir);
    snprintf(s->qemu_log_path, sizeof(s->qemu_log_path), "%.900s/qemu.log", s->log_dir);
    snprintf(s->virtiofsd_log_path, sizeof(s->virtiofsd_log_path), "%.900s/virtiofsd.log", s->log_dir);

    return 0;
}

/** @brief Acquire only the lifetime lease; caller serializes named config handoff. */
static int acquire_lease(daemon_state_t *s) {
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (s->lock_fd >= 0) {
        return 0; /* Already held */
    }

    int fd = open(s->lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_OFD_SETLK, &fl) != 0) {
        int saved_errno = errno;
        close(fd);
        if (saved_errno == EACCES || saved_errno == EAGAIN) {
            errno = EADDRINUSE;
        } else {
            errno = saved_errno;
        }
        return -1;
    }

    struct stat lock_stat;
    if (fstat(fd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_uid != getuid() || (lock_stat.st_mode & 077) != 0) {
        close(fd);
        errno = EACCES;
        return -1;
    }

    /* Record PID in lockfile */
    if (ftruncate(fd, 0) == 0) {
        char pid_buf[32];
        int n = snprintf(pid_buf, sizeof(pid_buf), "%d\n", (int)getpid());
        if (n > 0) {
            (void)write(fd, pid_buf, (size_t)n);
        }
    }

    s->lock_fd = fd;
    return 0;
}

int daemon_state_acquire_lock(daemon_state_t *s) {
    if (s == NULL) { errno = EINVAL; return -1; }
    if (s->lock_fd >= 0) return 0;
    const char *slash = strrchr(s->runtime_dir, '/');
    const char *name = slash == NULL ? s->runtime_dir : slash + 1;
    char expected[WaddleMaxPathLen];
    int named = daemon_device_get_socket_path(name, expected, sizeof(expected)) == 0 &&
                strcmp(expected, s->daemon_sock_path) == 0;
    if (!named) return acquire_lease(s);

    /* OFD read locks can nest safely: closing discovery's fd does not release
     * this outer lock. Writers cannot change the config before lease ownership. */
    int registry_fd = daemon_device_registry_lock(0);
    if (registry_fd < 0) return -1;
    device_info_t info;
    int result = daemon_device_find(name, &info);
    if (result == 0 && !info.config_valid) { errno = EINVAL; result = -1; }
    if (result == 0) {
        daemon_config_t config;
        daemon_config_init_defaults(&config);
        result = daemon_config_load_file(&config, info.config_path);
        if (result == 0) {
            result = acquire_lease(s);
            if (result == 0) {
                s->config = config;
                int written = snprintf(s->log_dir, sizeof(s->log_dir), "%s/logs", info.state_dir);
                if (written < 0 || (size_t)written >= sizeof(s->log_dir)) { errno = ENAMETOOLONG; result = -1; }
                else result = ensure_dir(s->log_dir);
                if (result == 0) {
                    snprintf(s->daemon_log_path, sizeof(s->daemon_log_path), "%.900s/daemon.log", s->log_dir);
                    snprintf(s->qemu_log_path, sizeof(s->qemu_log_path), "%.900s/qemu.log", s->log_dir);
                    snprintf(s->virtiofsd_log_path, sizeof(s->virtiofsd_log_path), "%.900s/virtiofsd.log", s->log_dir);
                }
            }
        }
    }
    int saved = errno;
    close(registry_fd);
    errno = saved;
    return result;
}

void daemon_state_release_lock(daemon_state_t *s) {
    if (s == NULL || s->lock_fd < 0) return;

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    (void)fcntl(s->lock_fd, F_OFD_SETLK, &fl);
    close(s->lock_fd);
    s->lock_fd = -1;
}

int daemon_state_start_subsystem(daemon_state_t *s, uint32_t flags, uint32_t timeout_sec) {
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (s->state == SubsystemStateRunning) {
        return 0; /* Already running */
    }

    if (s->state != SubsystemStateStopped && s->state != SubsystemStateFailed) {
        snprintf(s->last_error, sizeof(s->last_error),
                 "Subsystem is in transition state: %s",
                 waddle_subsystem_state_to_string(s->state));
        errno = EBUSY;
        return -1;
    }

    uint32_t wait_timeout = (timeout_sec > 0) ? timeout_sec : s->config.start_timeout_sec;
    (void)wait_timeout;
    (void)flags;

    if (s->mock_guest_sock_path[0] != '\0') {
        /* Skip VirtIO-FS and QEMU for mock environments */
        goto probe_readiness;
    }

    /* Step 1: VirtIO-FS host daemon */
    s->state = SubsystemStateStartingVirtiofs;
    if (s->config.mount_count > 0 && s->config.mounts[0].host_path[0] != '\0') {
        const char *shared_dir = s->config.mounts[0].host_path;
        char virtiofsd_bin[WaddleMaxPathLen];
        if (daemon_fs_find_binary(virtiofsd_bin, sizeof(virtiofsd_bin)) == 0) {
            if (daemon_fs_spawn(&s->virtiofs, s->virtiofsd_sock_path, shared_dir,
                                s->virtiofsd_log_path, virtiofsd_bin, 3000) != 0) {
                snprintf(s->last_error, sizeof(s->last_error),
                         "Failed to spawn virtiofsd: %.100s: %.100s",
                         shared_dir, strerror(errno));
                s->state = SubsystemStateFailed;
                return -1;
            }
        }
    }

    /* Step 2: QEMU hypervisor */
    s->state = SubsystemStateStartingQemu;
    const char *vfs_sock = (s->virtiofs.is_running) ? s->virtiofsd_sock_path : NULL;
    char qemu_bin[WaddleMaxPathLen];
    const char *target_qemu_bin = NULL;
    if (qemu_find_binary(qemu_bin, sizeof(qemu_bin)) == 0) {
        target_qemu_bin = qemu_bin;
    }
    if (qemu_spawn(&s->qemu, &s->config, s->qmp_sock_path, vfs_sock, s->qemu_log_path, target_qemu_bin) != 0) {
        snprintf(s->last_error, sizeof(s->last_error),
                 "Failed to spawn QEMU hypervisor: %.150s", strerror(errno));
        if (s->virtiofs.is_running) {
            daemon_fs_stop(&s->virtiofs, 1000);
        }
        s->state = SubsystemStateFailed;
        return -1;
    }

probe_readiness:
    /* Step 3: Probe Guest Readiness */
    s->state = SubsystemStateWaitingGuest;
    if (s->mock_guest_sock_path[0] != '\0') {
        /* In test environments, verify mock guest socket */
        int tries = 0;
        int connected = 0;
        while (tries < 20) {
            if (access(s->mock_guest_sock_path, F_OK) == 0) {
                connected = 1;
                break;
            }
            usleep(50000);
            tries++;
        }
        if (!connected) {
            snprintf(s->last_error, sizeof(s->last_error), "Timed out waiting for mock guest agent");
            s->state = SubsystemStateFailed;
            return -1;
        }
    } else if (flags & DaemonStartFlagWaitGuest) {
        int connected = 0;
        uint64_t start = state_monotonic_ms();
        while (state_monotonic_ms() - start < wait_timeout * 1000) {
            if (qemu_poll_status(&s->qemu) == 0) {
                snprintf(s->last_error, sizeof(s->last_error), "QEMU hypervisor exited prematurely");
                s->state = SubsystemStateFailed;
                return -1;
            }

            int sock = socket(AF_VSOCK, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_vm addr;
                memset(&addr, 0, sizeof(addr));
                addr.svm_family = AF_VSOCK;
                addr.svm_cid = s->config.vsock_cid;
                addr.svm_port = s->config.vsock_port;
                
                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    connected = 1;
                    close(sock);
                    break;
                }
                close(sock);
            }
            usleep(100000); /* 100ms */
        }
        if (!connected) {
            snprintf(s->last_error, sizeof(s->last_error), "Timed out waiting for VSOCK guest agent");
            s->state = SubsystemStateFailed;
            return -1;
        }
    }

    /* Step 4: Subsystem running */
    s->state = SubsystemStateRunning;
    s->running_since_sec = (uint64_t)time(NULL);
    s->last_error[0] = '\0';
    return 0;
}

int daemon_state_stop_subsystem(daemon_state_t *s, uint32_t force, uint32_t timeout_sec) {
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (s->state == SubsystemStateStopped) {
        return 0;
    }

    s->state = SubsystemStateStopping;

    if (force) {
        return daemon_state_kill_subsystem(s);
    }

    uint32_t stop_timeout = (timeout_sec > 0) ? timeout_sec : s->config.stop_timeout_sec;

    /* Issue ACPI shutdown via QMP */
    if (s->qemu.pid > 0) {
        (void)qemu_shutdown_graceful(&s->qemu);

        /* Wait for QEMU exit */
        uint32_t elapsed_ms = 0;
        while (elapsed_ms < stop_timeout * 1000U) {
            if (qemu_poll_status(&s->qemu) == 0) {
                break;
            }
            usleep(50000);
            elapsed_ms += 50;
        }

        /* Force kill if still running after timeout */
        if (s->qemu.is_running) {
            (void)qemu_kill(&s->qemu);
        }
    }

    /* Terminate VirtIO-FS */
    if (s->virtiofs.is_running) {
        (void)daemon_fs_stop(&s->virtiofs, 2000);
    }

    /* Unlink runtime sockets */
    unlink(s->qmp_sock_path);
    unlink(s->virtiofsd_sock_path);

    s->state = SubsystemStateStopped;
    s->running_since_sec = 0;
    s->last_error[0] = '\0';
    return 0;
}

int daemon_state_kill_subsystem(daemon_state_t *s) {
    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (s->qemu.pid > 0) {
        (void)qemu_kill(&s->qemu);
    }
    if (s->virtiofs.pid > 0) {
        (void)daemon_fs_stop(&s->virtiofs, 0);
    }

    unlink(s->qmp_sock_path);
    unlink(s->virtiofsd_sock_path);

    s->state = SubsystemStateStopped;
    s->running_since_sec = 0;
    return 0;
}

void daemon_state_get_status(const daemon_state_t *s, waddle_daemon_status_resp_t *resp) {
    if (s == NULL || resp == NULL) return;

    memset(resp, 0, sizeof(*resp));
    resp->subsystem_state = (uint32_t)s->state;
    resp->daemon_pid = (uint32_t)getpid();
    resp->qemu_pid = (uint32_t)s->qemu.pid;
    resp->virtiofsd_pid = (uint32_t)s->virtiofs.pid;
    resp->vsock_cid = s->config.vsock_cid;
    resp->vsock_port = s->config.vsock_port;

    if (s->state == SubsystemStateRunning && s->running_since_sec > 0) {
        uint64_t now = (uint64_t)time(NULL);
        resp->uptime_sec = (now >= s->running_since_sec) ? (now - s->running_since_sec) : 0;
    } else {
        resp->uptime_sec = 0;
    }

    resp->memory_mb = s->config.memory_mb;
    resp->vcpus = s->config.vcpus;
    resp->mount_count = s->config.mount_count;

    size_t count = (s->config.mount_count < WaddleMaxMounts) ? s->config.mount_count : WaddleMaxMounts;
    for (size_t i = 0; i < count; i++) {
        memcpy(&resp->mounts[i], &s->config.mounts[i], sizeof(waddle_daemon_fs_mount_t));
    }
}

void daemon_state_reap_children(daemon_state_t *s) {
    if (s == NULL) return;

    if (s->qemu.pid > 0) {
        if (qemu_poll_status(&s->qemu) == 0) {
            if (s->state == SubsystemStateRunning) {
                s->state = SubsystemStateStopped;
                s->running_since_sec = 0;
            }
        }
    }

    if (s->virtiofs.pid > 0) {
        (void)daemon_fs_poll_status(&s->virtiofs);
    }
}

void daemon_state_cleanup(daemon_state_t *s) {
    if (s == NULL) return;

    if (s->state != SubsystemStateStopped) {
        (void)daemon_state_kill_subsystem(s);
    }

    qemu_cleanup(&s->qemu);
    daemon_fs_cleanup(&s->virtiofs);
    /* A failed competing startup owns none of these socket names. */
    if (s->lock_fd >= 0) {
        unlink(s->daemon_sock_path);
        unlink(s->qmp_sock_path);
        unlink(s->virtiofsd_sock_path);
        daemon_state_release_lock(s);
    }
}
