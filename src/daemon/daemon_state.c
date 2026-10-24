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

static virtiofs_process_t *mount_process(daemon_state_t *s, size_t index) {
    return index == 0 ? &s->virtiofs : &s->virtiofs_additional[index - 1];
}

static const char *mount_socket_path(const daemon_state_t *s, size_t index) {
    return index == 0 ? s->virtiofsd_sock_path : s->virtiofsd_additional_sock_path[index - 1];
}

static const char *mount_log_path(const daemon_state_t *s, size_t index) {
    return index == 0 ? s->virtiofsd_log_path : s->virtiofsd_additional_log_path[index - 1];
}

static int set_virtiofs_log_paths(daemon_state_t *s) {
    size_t base_length = strlen(s->log_dir);
    static const char legacy_suffix[] = "/virtiofsd.log";
    if (base_length + sizeof(legacy_suffix) > WaddleMaxPathLen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(s->virtiofsd_log_path, s->log_dir, base_length);
    memcpy(s->virtiofsd_log_path + base_length, legacy_suffix, sizeof(legacy_suffix));
    for (size_t i = 1; i < WaddleMaxMounts; i++) {
        char suffix[32];
        int suffix_written = snprintf(suffix, sizeof(suffix), "/virtiofsd-%zu.log", i);
        if (suffix_written < 0 || (size_t)suffix_written >= sizeof(suffix) ||
            base_length + (size_t)suffix_written >= WaddleMaxPathLen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(s->virtiofsd_additional_log_path[i - 1], s->log_dir, base_length);
        memcpy(s->virtiofsd_additional_log_path[i - 1] + base_length, suffix,
               (size_t)suffix_written + 1);
    }
    return 0;
}

static char normalized_drive_letter(const char *guest_drive) {
    char letter = guest_drive[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    return letter;
}

static int mount_process_owned(const virtiofs_process_t *proc) {
    return proc->pid > 0 || proc->log_fd >= 0 || proc->socket_path[0] != '\0' ||
           proc->shared_dir[0] != '\0' || proc->log_path[0] != '\0';
}

static int mount_configs_equal(const daemon_state_t *s) {
    if (s->config.mount_count != s->virtiofs_owned_count) return 0;
    for (size_t i = 0; i < s->config.mount_count; i++) {
        const waddle_daemon_fs_mount_t *current = &s->config.mounts[i];
        const waddle_daemon_fs_mount_t *saved = &s->virtiofs_mount_config[i];
        if (strcmp(current->host_path, saved->host_path) != 0 ||
            strcmp(current->guest_drive, saved->guest_drive) != 0 ||
            current->read_only != saved->read_only) return 0;
    }
    return 1;
}

static void remember_mount_configs(daemon_state_t *s) {
    memset(s->virtiofs_mount_config, 0, sizeof(s->virtiofs_mount_config));
    if (s->config.mount_count <= WaddleMaxMounts) {
        memcpy(s->virtiofs_mount_config, s->config.mounts,
               s->config.mount_count * sizeof(s->config.mounts[0]));
    }
}

/* Stop in reverse order. A failed stop keeps its entire record and owned prefix. */
static int stop_mounts(daemon_state_t *s, uint32_t timeout_ms) {
    int first_error = 0;
    for (size_t i = s->virtiofs_owned_count; i > 0; i--) {
        virtiofs_process_t *proc = mount_process(s, i - 1);
        if (mount_process_owned(proc)) {
            if (daemon_fs_stop(proc, timeout_ms) != 0) {
                if (first_error == 0) first_error = errno != 0 ? errno : EIO;
                break;
            } else {
                errno = 0;
                daemon_fs_cleanup(proc);
                if (mount_process_owned(proc) && first_error == 0) first_error = errno != 0 ? errno : EIO;
            }
        }
        /* Preserve a contiguous owned prefix after the first teardown error. */
        if (first_error != 0) break;
    }
    while (s->virtiofs_owned_count > 0 &&
           !mount_process_owned(mount_process(s, s->virtiofs_owned_count - 1))) {
        s->virtiofs_owned_count--;
    }
    if (s->virtiofs_owned_count == 0) memset(s->virtiofs_mount_config, 0, sizeof(s->virtiofs_mount_config));
    if (first_error != 0) { errno = first_error; return -1; }
    return 0;
}

/* Close QEMU's owned log/QMP resources only after its child is confirmed dead. */
static int cleanup_dead_qemu(daemon_state_t *s) {
    if (s->qemu.pid > 0 || s->qemu.is_running) {
        errno = EBUSY;
        return -1;
    }
    if (s->qemu.log_fd < 0 && s->qemu.qmp.socket_fd < 0) return 0;
    qemu_cleanup(&s->qemu);
    return 0;
}

/* Release QEMU first; AV mappings remain owned while its process may still run. */
static int stop_qemu_and_mounts(daemon_state_t *s, uint32_t timeout_ms, int force) {
    if (s->qemu.pid > 0 || s->qemu.is_running) {
        if (force) {
            if (qemu_kill(&s->qemu) != 0) return -1;
        } else {
            (void)qemu_shutdown_graceful(&s->qemu);
            uint64_t deadline = state_monotonic_ms() + timeout_ms;
            while (s->qemu.is_running && state_monotonic_ms() < deadline) {
                int status = qemu_poll_status(&s->qemu);
                if (status == 0) break;
                if (status < 0) return -1;
                usleep(50000);
            }
            if (s->qemu.is_running) {
                if (!force) { errno = ETIMEDOUT; return -1; }
                if (qemu_kill(&s->qemu) != 0) return -1;
            }
        }
        if (s->qemu.is_running || s->qemu.pid > 0) { errno = EBUSY; return -1; }
    }
    if (cleanup_dead_qemu(s) != 0) return -1;
    if (stop_mounts(s, 2000) != 0) return -1;
    av_environment_free(&s->av_environment);
    unlink(s->qmp_sock_path);
    return 0;
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
    s->av_environment.fd = -1;
    s->qemu.pid = 0;
    s->qemu.log_fd = -1;
    s->qemu.qmp.socket_fd = -1;
    s->virtiofs.pid = 0;
    s->virtiofs.log_fd = -1;
    for (size_t i = 0; i < WaddleMaxMounts - 1; i++) {
        s->virtiofs_additional[i].pid = 0;
        s->virtiofs_additional[i].log_fd = -1;
    }

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
    for (size_t i = 1; i < WaddleMaxMounts; i++) {
        char suffix[32];
        int suffix_written = snprintf(suffix, sizeof(suffix), "/virtiofsd-%zu.sock", i);
        size_t base_length = strlen(s->runtime_dir);
        if (suffix_written < 0 || (size_t)suffix_written >= sizeof(suffix) ||
            base_length + (size_t)suffix_written >= WaddleMaxPathLen) { errno = ENAMETOOLONG; return -1; }
        memcpy(s->virtiofsd_additional_sock_path[i - 1], s->runtime_dir, base_length);
        memcpy(s->virtiofsd_additional_sock_path[i - 1] + base_length, suffix, (size_t)suffix_written + 1);
    }

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
    if (set_virtiofs_log_paths(s) != 0) return -1;

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
                    result = set_virtiofs_log_paths(s);
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

    if (s->config.mount_count > WaddleMaxMounts) { errno = EINVAL; return -1; }

    /* A readiness timeout retains the exact VM and mount identities. */
    if (s->qemu.is_running) {
        int alive = qemu_poll_status(&s->qemu);
        if (alive > 0) {
            if (!mount_configs_equal(s)) { errno = EBUSY; return -1; }
            for (size_t i = 0; i < s->virtiofs_owned_count; i++) {
                virtiofs_process_t *proc = mount_process(s, i);
                if (proc->pid > 0 && daemon_fs_poll_status(proc) <= 0) {
                    snprintf(s->last_error, sizeof(s->last_error), "virtiofsd mount %zu exited while QEMU remained alive", i);
                    (void)stop_qemu_and_mounts(s, 0, 1);
                    s->state = SubsystemStateFailed;
                    return -1;
                }
            }
            goto probe_readiness;
        }
        if (alive < 0) return -1;
        if (stop_qemu_and_mounts(s, 0, 1) != 0) return -1;
    }

    if (!s->qemu.is_running && s->qemu.pid > 0) {
        int alive = qemu_poll_status(&s->qemu);
        if (alive < 0) return -1;
        if (alive > 0) { errno = EBUSY; return -1; }
    }
    if (!s->qemu.is_running && s->qemu.pid <= 0) {
        if (cleanup_dead_qemu(s) != 0) return -1;
        if (s->virtiofs_owned_count > 0) {
            if (stop_mounts(s, 1000) != 0) { s->state = SubsystemStateFailed; return -1; }
        }
        if (s->av_environment.fd >= 0 || s->av_environment.mapping != NULL) {
            av_environment_free(&s->av_environment);
        }
    }

    /* Validate all descriptors before starting any child. */
    for (size_t i = 0; i < s->config.mount_count; i++) {
        if (s->config.mounts[i].host_path[0] == '\0' ||
            s->config.mounts[i].guest_drive[0] == '\0' ||
            memchr(s->config.mounts[i].host_path, '\0', WaddleMaxPathLen) == NULL ||
            memchr(s->config.mounts[i].guest_drive, '\0', sizeof(s->config.mounts[i].guest_drive)) == NULL ||
            strnlen(s->config.mounts[i].guest_drive, sizeof(s->config.mounts[i].guest_drive)) != 3 ||
            !((s->config.mounts[i].guest_drive[0] >= 'A' && s->config.mounts[i].guest_drive[0] <= 'Z') ||
              (s->config.mounts[i].guest_drive[0] >= 'a' && s->config.mounts[i].guest_drive[0] <= 'z')) ||
            s->config.mounts[i].guest_drive[1] != ':' ||
            (s->config.mounts[i].guest_drive[2] != '/' && s->config.mounts[i].guest_drive[2] != '\\') ||
            s->config.mounts[i].read_only > 1) { errno = EINVAL; return -1; }
        for (size_t previous = 0; previous < i; previous++) {
            if (normalized_drive_letter(s->config.mounts[previous].guest_drive) ==
                normalized_drive_letter(s->config.mounts[i].guest_drive)) {
                errno = EINVAL;
                return -1;
            }
        }
    }

    char qemu_bin[WaddleMaxPathLen];
    if (qemu_find_binary(qemu_bin, sizeof(qemu_bin)) != 0) {
        int saved = errno;
        snprintf(s->last_error, sizeof(s->last_error), "QEMU is required to start the subsystem: %.100s", strerror(saved));
        s->state = SubsystemStateFailed;
        errno = saved;
        return -1;
    }

    /* Step 1: one virtiofsd per configured export, in config order. */
    s->state = SubsystemStateStartingVirtiofs;
    char virtiofsd_bin[WaddleMaxPathLen];
    if (s->config.mount_count > 0 && daemon_fs_find_binary(virtiofsd_bin, sizeof(virtiofsd_bin)) != 0) {
        snprintf(s->last_error, sizeof(s->last_error), "virtiofsd is required for configured mounts: %.100s", strerror(errno));
        s->state = SubsystemStateFailed;
        return -1;
    }
    if (s->virtiofs_owned_count == 0) remember_mount_configs(s);
    for (size_t i = s->virtiofs_owned_count; i < s->config.mount_count; i++) {
        virtiofs_process_t *proc = mount_process(s, i);
        if (daemon_fs_spawn_for_mount(proc, mount_socket_path(s, i), s->config.mounts[i].host_path,
                                      s->config.mounts[i].read_only, mount_log_path(s, i),
                                      virtiofsd_bin, 3000) != 0) {
            int saved = errno;
            if (mount_process_owned(proc)) s->virtiofs_owned_count = (uint32_t)i + 1;
            int cleanup_result = stop_mounts(s, 1000);
            if (cleanup_result != 0) {
                int cleanup_error = errno != 0 ? errno : EIO;
                snprintf(s->last_error, sizeof(s->last_error),
                         "Failed to spawn virtiofsd mount %zu: %.48s; mount cleanup failed: %.48s",
                         i, strerror(saved), strerror(cleanup_error));
                s->state = SubsystemStateFailed;
                errno = cleanup_error;
                return -1;
            }
            snprintf(s->last_error, sizeof(s->last_error), "Failed to spawn virtiofsd mount %zu: %.100s",
                     i, strerror(saved));
            s->state = SubsystemStateFailed;
            errno = saved;
            return -1;
        }
        s->virtiofs_owned_count = (uint32_t)i + 1;
    }

    /* Prepare IVSHMEM as part of this managed startup, before QEMU opens it. */
    if (s->config.av_enabled && av_environment_prepare(&s->av_environment, &s->config,
                                                       s->last_error, sizeof(s->last_error)) != 0) {
        int prepare_error = errno != 0 ? errno : EIO;
        char prepare_message[sizeof(s->last_error)];
        snprintf(prepare_message, sizeof(prepare_message), "%s", s->last_error);
        if (stop_mounts(s, 1000) != 0) {
            int cleanup_error = errno != 0 ? errno : EIO;
            snprintf(s->last_error, sizeof(s->last_error), "%.100s; mount cleanup failed: %.100s",
                     prepare_message, strerror(cleanup_error));
            s->state = SubsystemStateFailed;
            errno = cleanup_error;
            return -1;
        }
        av_environment_free(&s->av_environment);
        snprintf(s->last_error, sizeof(s->last_error), "%s", prepare_message);
        s->state = SubsystemStateFailed;
        errno = prepare_error;
        return -1;
    }

    /* Step 2: QEMU hypervisor */
    s->state = SubsystemStateStartingQemu;
    qemu_virtiofs_mount_t qemu_mounts[WaddleMaxMounts];
    memset(qemu_mounts, 0, sizeof(qemu_mounts));
    for (size_t i = 0; i < s->config.mount_count; i++) {
        snprintf(qemu_mounts[i].socket_path, sizeof(qemu_mounts[i].socket_path), "%s", mount_socket_path(s, i));
        memcpy(qemu_mounts[i].guest_drive, s->config.mounts[i].guest_drive, sizeof(qemu_mounts[i].guest_drive));
    }
    const char *target_qemu_bin = NULL;
    target_qemu_bin = qemu_bin;
    /* Only canonical registered-device socket identity supplies a sink suffix.
     * Unmanaged legacy runtimes retain the documented default namespace. */
    const char *qemu_device_name = "default";
    const char *runtime_name = strrchr(s->runtime_dir, '/');
    char named_socket[WaddleMaxPathLen];
    if (runtime_name != NULL &&
        daemon_device_get_socket_path(runtime_name + 1, named_socket, sizeof(named_socket)) == 0 &&
        strcmp(named_socket, s->daemon_sock_path) == 0) {
        qemu_device_name = runtime_name + 1;
    }
    if (qemu_spawn_for_device_mounts(&s->qemu, &s->config, qemu_device_name,
                                    s->qmp_sock_path,
                                    s->config.mount_count == 0 ? NULL : qemu_mounts,
                                    s->config.mount_count,
                                    s->qemu_log_path, target_qemu_bin) != 0) {
        int spawn_error = errno != 0 ? errno : EIO;
        if (stop_mounts(s, 1000) != 0) {
            int cleanup_error = errno != 0 ? errno : EIO;
            snprintf(s->last_error, sizeof(s->last_error),
                     "Failed to spawn QEMU hypervisor: %.48s; mount cleanup failed: %.48s",
                     strerror(spawn_error), strerror(cleanup_error));
            s->state = SubsystemStateFailed;
            errno = cleanup_error;
            return -1;
        }
        av_environment_free(&s->av_environment);
        snprintf(s->last_error, sizeof(s->last_error),
                 "Failed to spawn QEMU hypervisor: %.150s", strerror(spawn_error));
        s->state = SubsystemStateFailed;
        errno = spawn_error;
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
                (void)stop_qemu_and_mounts(s, 0, 1);
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

    if (s->state == SubsystemStateStopped && s->qemu.pid <= 0 && s->virtiofs_owned_count == 0) {
        if (cleanup_dead_qemu(s) != 0) return -1;
        return 0;
    }

    waddle_subsystem_state_t previous_state = s->state;
    s->state = SubsystemStateStopping;

    uint32_t stop_timeout = (timeout_sec > 0) ? timeout_sec : s->config.stop_timeout_sec;
    if (force && force != WaddleStopGracefulOnly) return daemon_state_kill_subsystem(s);
    if (s->qemu.pid > 0 && s->qemu.is_running) {
        (void)qemu_shutdown_graceful(&s->qemu);
        uint64_t deadline = state_monotonic_ms() + (uint64_t)stop_timeout * 1000;
        while (s->qemu.is_running && state_monotonic_ms() < deadline) {
            int status = qemu_poll_status(&s->qemu);
            if (status == 0) break;
            if (status < 0) { s->state = previous_state; return -1; }
            usleep(50000);
        }
        if (s->qemu.is_running) {
            if (force == WaddleStopGracefulOnly) {
                s->state = previous_state;
                snprintf(s->last_error, sizeof(s->last_error),
                         "Graceful shutdown timed out; VM and owned resources preserved");
                errno = ETIMEDOUT;
                return -1;
            }
            if (qemu_kill(&s->qemu) != 0) { s->state = SubsystemStateFailed; return -1; }
        }
    }
    if (s->qemu.is_running || s->qemu.pid > 0) { s->state = SubsystemStateFailed; errno = EBUSY; return -1; }
    if (cleanup_dead_qemu(s) != 0) { s->state = SubsystemStateFailed; return -1; }
    if (stop_mounts(s, 2000) != 0) { s->state = SubsystemStateFailed; return -1; }
    av_environment_free(&s->av_environment);
    unlink(s->qmp_sock_path);

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

    if ((s->qemu.pid > 0 || s->qemu.is_running) && qemu_kill(&s->qemu) != 0) {
        s->state = SubsystemStateFailed;
        return -1;
    }
    if (s->qemu.pid > 0 || s->qemu.is_running) { errno = EBUSY; s->state = SubsystemStateFailed; return -1; }
    if (cleanup_dead_qemu(s) != 0) { s->state = SubsystemStateFailed; return -1; }
    if (stop_mounts(s, 0) != 0) { s->state = SubsystemStateFailed; return -1; }
    av_environment_free(&s->av_environment);
    unlink(s->qmp_sock_path);

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

    int qemu_reaped = 0;
    if (s->qemu.pid > 0) {
        if (qemu_poll_status(&s->qemu) == 0) {
            qemu_reaped = 1;
            if (s->state == SubsystemStateRunning) {
                s->state = SubsystemStateStopped;
                s->running_since_sec = 0;
            }
            if (stop_qemu_and_mounts(s, 0, 1) != 0) s->state = SubsystemStateFailed;
        }
    }

    /* Release prepared AV state only when QEMU is confirmed absent. This also
     * covers startup failures where AV was prepared but no QEMU child spawned. */
    if (!qemu_reaped && s->qemu.pid <= 0 && !s->qemu.is_running) {
        if (cleanup_dead_qemu(s) != 0) {
            s->state = SubsystemStateFailed;
        } else if (stop_mounts(s, 0) == 0) {
            av_environment_free(&s->av_environment);
        } else {
            s->state = SubsystemStateFailed;
        }
    }

    /* Mount failure while QEMU lives is fail-closed: tear down QEMU first. */
    if (s->qemu.is_running) {
        for (size_t i = 0; i < s->virtiofs_owned_count; i++) {
            virtiofs_process_t *proc = mount_process(s, i);
            if (proc->pid > 0 && daemon_fs_poll_status(proc) == 0) {
                snprintf(s->last_error, sizeof(s->last_error), "virtiofsd mount %zu exited unexpectedly", i);
                if (stop_qemu_and_mounts(s, 0, 1) != 0) s->state = SubsystemStateFailed;
                else s->state = SubsystemStateFailed;
                return;
            }
        }
    }
}

void daemon_state_cleanup(daemon_state_t *s) {
    if (s == NULL) return;

    if (s->state != SubsystemStateStopped || s->qemu.pid > 0 || s->virtiofs_owned_count > 0) {
        if (daemon_state_kill_subsystem(s) != 0) return;
    }
    if (s->qemu.pid <= 0 && !s->qemu.is_running && stop_mounts(s, 0) == 0) {
        av_environment_free(&s->av_environment);
        (void)cleanup_dead_qemu(s);
    }
    /* A failed competing startup owns none of these socket names. */
    if (s->lock_fd >= 0) {
        unlink(s->daemon_sock_path);
        unlink(s->qmp_sock_path);
        daemon_state_release_lock(s);
    }
}
