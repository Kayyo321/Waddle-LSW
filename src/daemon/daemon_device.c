/**
 * @file daemon_device.c
 * @brief Implementation of device profile management, registry discovery, and storage initialization.
 */

#include "daemon_device.h"
#include "daemon_config.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Open each absolute directory component without following symlinks.
 * @param[in] path Borrowed absolute path; no dot components.
 * @param[in] create Nonzero creates absent components with mode 0700.
 * @return Owned CLOEXEC directory fd, or -1 with errno. Caller closes; thread-safe.
 */
int daemon_device_open_directory(const char *path, int create) {
    if (path == NULL || path[0] != '/' || strlen(path) >= WaddleMaxPathLen) {
        errno = EINVAL;
        return -1;
    }
    char copy[WaddleMaxPathLen];
    memcpy(copy, path, strlen(path) + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        int created = create ? mkdirat(fd, part, 0700) : -1;
        if (created == 0 && fsync(fd) != 0) {
            int saved = errno; close(fd); errno = saved; return -1;
        }
        if (create && created != 0 && errno != EEXIST) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        if (next < 0) { errno = saved; return -1; }
        fd = next;
    }
    return fd;
}

/** @brief Resolve an XDG root without touching disk; reject relative or missing roots. */
static const char *resolve_root(const char *variable, int *fallback) {
    const char *root = getenv(variable);
    *fallback = root == NULL || root[0] == '\0';
    if (*fallback) root = getenv("HOME");
    if (root == NULL || root[0] != '/') { errno = EINVAL; return NULL; }
    return root;
}

int daemon_device_validate_name(const char *name) {
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(name);
    if (len == 0 || len >= WaddleMaxDeviceNameLen) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')) && c != '-' && c != '_') {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

int daemon_device_get_config_dir(char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap == 0) { errno = EINVAL; return -1; }
    int fallback;
    const char *root = resolve_root("XDG_CONFIG_HOME", &fallback);
    if (root == NULL) return -1;
    int len = snprintf(out_path, path_cap, "%s%s/waddle/devices", root,
                       fallback ? "/.config" : "");
    if (len < 0 || (size_t)len >= path_cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int daemon_device_get_state_dir(const char *device_name, char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap == 0 || daemon_device_validate_name(device_name) != 0) {
        errno = EINVAL; return -1;
    }
    int fallback;
    const char *root = resolve_root("XDG_STATE_HOME", &fallback);
    if (root == NULL) return -1;
    int len = snprintf(out_path, path_cap, "%s%s/waddle/devices/%s", root,
                       fallback ? "/.local/state" : "", device_name);
    if (len < 0 || (size_t)len >= path_cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int daemon_device_get_socket_path(const char *device_name, char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap == 0 ||
        (device_name != NULL && daemon_device_validate_name(device_name) != 0)) {
        errno = EINVAL; return -1;
    }
    char runtime_dir[WaddleMaxPathLen];
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int len;
    if (xdg != NULL && xdg[0] != '\0') {
        if (xdg[0] != '/') { errno = EINVAL; return -1; }
        len = snprintf(runtime_dir, sizeof(runtime_dir), "%s/waddle", xdg);
    } else {
        len = snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/waddle-%u", (unsigned)getuid());
    }
    if (len < 0 || (size_t)len >= sizeof(runtime_dir)) { errno = ENAMETOOLONG; return -1; }
    len = device_name == NULL
        ? snprintf(out_path, path_cap, "%s/daemon.sock", runtime_dir)
        : snprintf(out_path, path_cap, "%s/%s/daemon.sock", runtime_dir, device_name);
    if (len < 0 || (size_t)len >= path_cap || (size_t)len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}

int daemon_device_find_base_disk(char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap < 16) {
        errno = EINVAL;
        return -1;
    }

    const char *home = getenv("HOME");
    if (home == NULL) home = "/home/dev";

    char candidate[WaddleMaxPathLen];

    /* Candidate 1: User's validation directory */
    snprintf(candidate, sizeof(candidate), "%s/.local/share/waddle-vm-validation/windows.qcow2", home);
    if (access(candidate, R_OK) == 0) {
        snprintf(out_path, path_cap, "%s", candidate);
        return 0;
    }

    /* Candidate 2: Local state vm location */
    snprintf(candidate, sizeof(candidate), "%s/.local/state/waddle/vm/windows.qcow2", home);
    if (access(candidate, R_OK) == 0) {
        snprintf(out_path, path_cap, "%s", candidate);
        return 0;
    }

    /* Candidate 3: System shared location */
    if (access("/var/lib/waddle/windows.qcow2", R_OK) == 0) {
        snprintf(out_path, path_cap, "/var/lib/waddle/windows.qcow2");
        return 0;
    }

    /* Candidate 4: Build directory */
    if (access("build/windows.qcow2", R_OK) == 0) {
        snprintf(out_path, path_cap, "build/windows.qcow2");
        return 0;
    }

    errno = ENOENT;
    return -1;
}

uint32_t daemon_device_allocate_cid(const device_list_t *list) {
    if (list == NULL || list->count > MaxDeviceCount) { errno = EINVAL; return 0; }
    uint32_t highest = BaseVsockCid - 1;
    for (size_t i = 0; i < list->count; i++) {
        uint32_t cid = list->devices[i].vsock_cid;
        if (cid < BaseVsockCid || cid == UINT32_MAX) { errno = EINVAL; return 0; }
        if (cid > highest) highest = cid;
    }
    if (highest < UINT32_MAX - 1) return highest + 1;
    for (uint32_t candidate = BaseVsockCid; candidate < UINT32_MAX; candidate++) {
        int used = 0;
        for (size_t i = 0; i < list->count; i++) {
            if (list->devices[i].vsock_cid == candidate) { used = 1; break; }
        }
        if (!used) return candidate;
    }
    errno = ENOSPC;
    return 0;
}

/** @brief Order profiles bytewise; comparator borrows both elements. */
static int compare_devices(const void *left, const void *right) {
    return strcmp(((const device_info_t *)left)->name, ((const device_info_t *)right)->name);
}

/** @brief Scan while caller owns registry lock; never create paths or probe sockets. */
int daemon_device_scan_locked(device_list_t *list) {
    memset(list, 0, sizeof(*list));
    char config_dir[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(config_dir, sizeof(config_dir)) != 0) return -1;
    int dir_fd = daemon_device_open_directory(config_dir, 0);
    if (dir_fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *dir = fdopendir(dir_fd);
    if (dir == NULL) { int saved = errno; close(dir_fd); errno = saved; return -1; }
    int result = -1;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 4 || strcmp(ent->d_name + len - 4, ".ini") != 0) continue;
        if (len - 4 >= WaddleMaxDeviceNameLen) { errno = EINVAL; goto cleanup; }
        if (list->count == MaxDeviceCount) { errno = ENOSPC; goto cleanup; }
        device_info_t *info = &list->devices[list->count];
        memcpy(info->name, ent->d_name, len - 4);
        info->name[len - 4] = '\0';
        if (daemon_device_validate_name(info->name) != 0) goto cleanup;
        int n = snprintf(info->config_path, sizeof(info->config_path), "%s/%s", config_dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(info->config_path)) { errno = ENAMETOOLONG; goto cleanup; }
        if (daemon_device_get_state_dir(info->name, info->state_dir, sizeof(info->state_dir)) != 0 ||
            daemon_device_get_socket_path(info->name, info->socket_path, sizeof(info->socket_path)) != 0) goto cleanup;
        int fd = openat(dir_fd, ent->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0) {
            struct stat st;
            char data[65537];
            ssize_t amount = -1;
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size <= 65536) {
                amount = read(fd, data, sizeof(data));
            }
            close(fd);
            daemon_config_t cfg;
            daemon_config_init_defaults(&cfg);
            cfg.vsock_cid = 0;
            cfg.disk_image[0] = '\0';
            if (amount >= 0 && amount <= 65536 && daemon_config_parse_string(&cfg, data, (size_t)amount) == 0) {
                memcpy(info->disk_image, cfg.disk_image, sizeof(info->disk_image));
                info->vsock_cid = cfg.vsock_cid;
                info->vsock_port = cfg.vsock_port;
                info->memory_mb = cfg.memory_mb;
                info->vcpus = cfg.vcpus;
                info->config_valid = 1;
            }
        }
        list->count++;
        errno = 0;
    }
    if (errno != 0) goto cleanup;
    qsort(list->devices, list->count, sizeof(list->devices[0]), compare_devices);
    result = (int)list->count;
cleanup:
    { int saved = errno; closedir(dir); errno = saved; }
    return result;
}

int daemon_device_registry_lock(int exclusive) {
    char path[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(path, sizeof(path)) != 0) return -1;
    char *slash = strrchr(path, '/');
    *slash = '\0';
    int dir_fd = daemon_device_open_directory(path, exclusive);
    if (dir_fd < 0) return -1;
    int fd = openat(dir_fd, "registry.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW |
                    (exclusive ? O_CREAT : 0), 0600);
    int saved = errno;
    close(dir_fd);
    if (fd < 0) { errno = saved; return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        close(fd); errno = EACCES; return -1;
    }
    struct flock lock = { .l_type = exclusive ? F_WRLCK : F_RDLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_OFD_SETLKW, &lock) != 0) { saved = errno; close(fd); errno = saved; return -1; }
    if (daemon_device_recover(exclusive) != 0) {
        saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}

int daemon_device_list(device_list_t *list) {
    if (list == NULL) { errno = EINVAL; return -1; }
    int fd = daemon_device_registry_lock(0);
    if (fd < 0 && errno != ENOENT) return -1;
    int result = daemon_device_scan_locked(list);
    int saved = errno;
    if (fd >= 0) close(fd);
    errno = saved;
    return result;
}

int daemon_device_find(const char *name, device_info_t *out_info) {
    if (name == NULL || out_info == NULL) {
        errno = EINVAL;
        return -1;
    }

    device_list_t list;
    if (daemon_device_list(&list) < 0) {
        return -1;
    }

    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.devices[i].name, name) == 0) {
            memcpy(out_info, &list.devices[i], sizeof(*out_info));
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}

int daemon_device_init(const char *name, const char *custom_base_disk, device_info_t *out_info) {
    device_request_t request = { .operation = DeviceInit, .name = name, .argument = custom_base_disk };
    char output[WaddleMaxPathLen];
    if (daemon_device_mutate(&request, output) != 0) return -1;
    return out_info == NULL ? 0 : daemon_device_find(name, out_info);
}

/** @brief Open private registry parent; caller closes owned fd. Never follows links. */
static int open_registry_parent(int create) {
    char path[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(path, sizeof(path)) != 0) return -1;
    *strrchr(path, '/') = '\0';
    int fd = daemon_device_open_directory(path, create);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

/** @brief Read an optional default while registry lock is held; no allocation. */
static int read_default_unlocked(char *name, size_t capacity) {
    name[0] = '\0';
    int dir_fd = open_registry_parent(0);
    if (dir_fd < 0) return errno == ENOENT ? 0 : -1;
    int fd = openat(dir_fd, "default_device", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int saved = errno;
    close(dir_fd);
    if (fd < 0) { errno = saved; return saved == ENOENT ? 0 : -1; }
    struct stat st;
    char data[WaddleMaxDeviceNameLen + 1];
    ssize_t len = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
        (st.st_mode & 077) == 0 && st.st_size > 0 && st.st_size <= WaddleMaxDeviceNameLen) {
        len = read(fd, data, sizeof(data));
    }
    saved = errno;
    close(fd);
    if (len <= 0 || len >= (ssize_t)sizeof(data) || data[len - 1] != '\n') {
        errno = len < 0 && saved ? saved : EINVAL; return -1;
    }
    data[--len] = '\0';
    if (memchr(data, '\0', (size_t)len) != NULL || daemon_device_validate_name(data) != 0) {
        errno = EINVAL; return -1;
    }
    if ((size_t)len >= capacity) { errno = ENAMETOOLONG; return -1; }
    memcpy(name, data, (size_t)len + 1);
    return 0;
}

int daemon_device_default_get(char *name, size_t capacity) {
    if (name == NULL || capacity == 0) { errno = EINVAL; return -1; }
    name[0] = '\0';
    int lock_fd = daemon_device_registry_lock(0);
    if (lock_fd < 0) return errno == ENOENT ? 0 : -1;
    int result = read_default_unlocked(name, capacity);
    int saved = errno;
    close(lock_fd);
    errno = saved;
    return result;
}

int daemon_device_default_set(const char *name) {
    if (name != NULL && daemon_device_validate_name(name) != 0) return -1;
    int lock_fd = daemon_device_registry_lock(1);
    if (lock_fd < 0) return -1;
    int result = -1;
    int dir_fd = -1;
    int fd = -1;
    char stage[80] = {0};
    device_list_t list;
    if (name != NULL) {
        if (daemon_device_scan_locked(&list) < 0) goto cleanup;
        size_t i;
        for (i = 0; i < list.count; i++) {
            if (strcmp(name, list.devices[i].name) == 0) break;
        }
        if (i == list.count) { errno = ENOENT; goto cleanup; }
        if (!list.devices[i].config_valid) { errno = EINVAL; goto cleanup; }
        struct stat disk;
        if (stat(list.devices[i].disk_image, &disk) != 0) goto cleanup;
        if (!S_ISREG(disk.st_mode)) { errno = EINVAL; goto cleanup; }
    }
    dir_fd = open_registry_parent(0);
    if (dir_fd < 0) goto cleanup;
    if (name == NULL) {
        if (unlinkat(dir_fd, "default_device", 0) != 0 && errno != ENOENT) goto cleanup;
        result = fsync(dir_fd);
        goto cleanup;
    }
    for (unsigned i = 0; i < 100; i++) {
        snprintf(stage, sizeof(stage), ".default-%ld-%u", (long)getpid(), i);
        fd = openat(dir_fd, stage, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { stage[0] = '\0'; goto cleanup; }
    char data[WaddleMaxDeviceNameLen + 1];
    int len = snprintf(data, sizeof(data), "%s\n", name);
    if (write(fd, data, (size_t)len) != len || fsync(fd) != 0) goto cleanup;
    if (renameat(dir_fd, stage, dir_fd, "default_device") != 0) goto cleanup;
    stage[0] = '\0';
    result = fsync(dir_fd);
cleanup:
    { int saved = errno;
      if (fd >= 0) close(fd);
      if (stage[0] && dir_fd >= 0) unlinkat(dir_fd, stage, 0);
      if (dir_fd >= 0) close(dir_fd);
      close(lock_fd);
      errno = saved; }
    return result;
}


/** @brief Refuse a disk still open by any same-user process, including orphan QEMU.
 * Borrow info; use stat identity rather than PID text or basename guesses. Disappeared
 * processes/fds are harmless; inaccessible live same-user fd tables fail closed.
 */
static int check_disk_holders(const device_info_t *info) {
    struct stat disk;
    if (stat(info->disk_image, &disk) != 0) return -1;
    if (!S_ISREG(disk.st_mode)) { errno = EINVAL; return -1; }
    DIR *processes = opendir("/proc");
    if (processes == NULL) return -1;
    int result = -1;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(processes)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        struct stat owner;
        if (fstatat(dirfd(processes), entry->d_name, &owner, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) { errno = 0; continue; }
            goto cleanup;
        }
        if (owner.st_uid != getuid()) continue;
        char path[300];
        int n = snprintf(path, sizeof(path), "/proc/%s/fd", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { errno = EOVERFLOW; goto cleanup; }
        DIR *fds = opendir(path);
        if (fds == NULL) {
            if (errno == ENOENT || errno == EACCES) { errno = 0; continue; }
            goto cleanup;
        }
        struct dirent *file;
        int failed = 0;
        errno = 0;
        while ((file = readdir(fds)) != NULL) {
            if (file->d_name[0] == '.') continue;
            struct stat target;
            if (fstatat(dirfd(fds), file->d_name, &target, 0) != 0) {
                if (errno == ENOENT || errno == EACCES) { errno = 0; continue; }
                failed = 1; break;
            }
            if (target.st_dev == disk.st_dev && target.st_ino == disk.st_ino) {
                errno = EBUSY; failed = 1; break;
            }
        }
        if (errno != 0) failed = 1;
        int saved = errno;
        closedir(fds);
        errno = saved;
        if (failed) goto cleanup;
    }
    if (errno == 0) result = 0;
cleanup:
    { int saved = errno; closedir(processes); errno = saved; }
    return result;
}

/** @brief Probe quiescence under registry ownership; retain existing lease until close.
 * No runtime paths are created. Socket names remain conservatively busy even if
 * their supervisor disappeared; a later diagnostic recovery must establish ownership.
 */
int daemon_device_lock_quiescent(const device_info_t *info, int *lease_fd) {
    *lease_fd = -1;
    char runtime[WaddleMaxPathLen];
    memcpy(runtime, info->socket_path, strlen(info->socket_path) + 1);
    *strrchr(runtime, '/') = '\0';
    int dir_fd = daemon_device_open_directory(runtime, 0);
    if (dir_fd < 0 && errno != ENOENT) return -1;
    if (dir_fd >= 0) {
        struct stat st;
        if (fstat(dir_fd, &st) != 0 || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
            close(dir_fd); errno = EACCES; return -1;
        }
        int fd = openat(dir_fd, "waddle.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0 && errno != ENOENT) { int saved = errno; close(dir_fd); errno = saved; return -1; }
        if (fd >= 0) {
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
                close(fd); close(dir_fd); errno = EACCES; return -1;
            }
            struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
            if (fcntl(fd, F_OFD_SETLK, &lock) != 0) {
                int saved = errno == EAGAIN || errno == EACCES ? EBUSY : errno;
                close(fd); close(dir_fd); errno = saved; return -1;
            }
            *lease_fd = fd;
        }
        const char *sockets[] = {"daemon.sock", "qmp.sock", "virtiofsd.sock"};
        for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
            if (fstatat(dir_fd, sockets[i], &st, AT_SYMLINK_NOFOLLOW) == 0) {
                close(dir_fd); errno = EBUSY; return -1;
            }
            if (errno != ENOENT) { int saved = errno; close(dir_fd); errno = saved; return -1; }
        }
        close(dir_fd);
    }
    if (check_disk_holders(info) != 0) return -1;
    /* qemu-img check obtains an exclusive write lock: protected proc entries
     * cannot hide a live QEMU writer from this independent kernel-backed check. */
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        int sink = open("/dev/null", O_WRONLY);
        if (sink >= 0) { dup2(sink, 1); dup2(sink, 2); close(sink); }
        execlp("qemu-img", "qemu-img", "check", "-f", "qcow2", info->disk_image, (char *)NULL);
        _exit(127);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { errno = EBUSY; return -1; }
    return 0;
}

int daemon_device_config_update(const char *name, const char *const *changes,
                                size_t count, int reset, int dry_run) {
    if (daemon_device_validate_name(name) != 0 || changes == NULL || count == 0 || count > 6) {
        errno = EINVAL; return -1;
    }
    /* A dry run takes a shared existing lock; real runs serialize replacement. */
    int registry_fd = daemon_device_registry_lock(!dry_run);
    if (registry_fd < 0) return -1;
    int result = -1, lease_fd = -1, dir_fd = -1, input_fd = -1, output_fd = -1;
    char stage[80] = {0};
    device_list_t list;
    if (daemon_device_scan_locked(&list) < 0) goto cleanup;
    device_info_t *info = NULL;
    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.devices[i].name, name) == 0) { info = &list.devices[i]; break; }
    }
    if (info == NULL) { errno = ENOENT; goto cleanup; }
    if (!info->config_valid) { errno = EINVAL; goto cleanup; }
    char directory[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(directory, sizeof(directory)) != 0) goto cleanup;
    dir_fd = daemon_device_open_directory(directory, 0);
    if (dir_fd < 0) goto cleanup;
    char filename[WaddleMaxDeviceNameLen + 4];
    snprintf(filename, sizeof(filename), "%s.ini", name);
    input_fd = openat(dir_fd, filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (input_fd < 0) goto cleanup;
    struct stat st;
    if (fstat(input_fd, &st) != 0) goto cleanup;
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0 ||
        st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 65536) { errno = EACCES; goto cleanup; }
    char input[65537], output[65536];
    size_t length = 0, output_length = 0;
    while (length < sizeof(input)) {
        ssize_t amount = read(input_fd, input + length, sizeof(input) - length);
        if (amount < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (amount == 0) break;
        length += (size_t)amount;
    }
    if (length != (size_t)st.st_size || daemon_config_edit(input, length, changes, count, reset,
                                                         output, sizeof(output), &output_length) != 0) {
        errno = EINVAL; goto cleanup;
    }
    if (daemon_device_lock_quiescent(info, &lease_fd) != 0) goto cleanup;
    if (dry_run) { result = 0; goto cleanup; }
    for (unsigned i = 0; i < 100; i++) {
        snprintf(stage, sizeof(stage), ".config-%ld-%u", (long)getpid(), i);
        output_fd = openat(dir_fd, stage, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output_fd >= 0 || errno != EEXIST) break;
    }
    if (output_fd < 0) { stage[0] = '\0'; goto cleanup; }
    size_t written = 0;
    while (written < output_length) {
        ssize_t amount = write(output_fd, output + written, output_length - written);
        if (amount < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (amount == 0) { errno = EIO; goto cleanup; }
        written += (size_t)amount;
    }
    if (fsync(output_fd) != 0) goto cleanup;
    struct stat current;
    if (fstatat(dir_fd, filename, &current, AT_SYMLINK_NOFOLLOW) != 0) goto cleanup;
    if (current.st_dev != st.st_dev || current.st_ino != st.st_ino) { errno = EBUSY; goto cleanup; }
    if (renameat(dir_fd, stage, dir_fd, filename) != 0) goto cleanup;
    stage[0] = '\0';
    result = fsync(dir_fd);
cleanup:
    { int saved = errno;
      if (input_fd >= 0) close(input_fd);
      if (output_fd >= 0) close(output_fd);
      if (stage[0] && dir_fd >= 0) unlinkat(dir_fd, stage, 0);
      if (dir_fd >= 0) close(dir_fd);
      if (lease_fd >= 0) close(lease_fd);
      close(registry_fd);
      errno = saved; }
    return result;
}
