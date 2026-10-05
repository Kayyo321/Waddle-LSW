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

/**
 * @brief Recursively ensures that a directory and its parents exist with 0755 permissions.
 *
 * @param[in] path Directory path.
 * @return 0 on success, or -1 on error.
 */
static int recursive_mkdir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    char tmp[WaddleMaxPathLen];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
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
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

int daemon_device_get_config_dir(char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap < 32) {
        errno = EINVAL;
        return -1;
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        if (snprintf(out_path, path_cap, "%s/waddle/devices", xdg) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        const char *home = getenv("HOME");
        if (home == NULL) home = "/root";
        if (snprintf(out_path, path_cap, "%s/.config/waddle/devices", home) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    return recursive_mkdir(out_path);
}

int daemon_device_get_state_dir(const char *device_name, char *out_path, size_t path_cap) {
    if (device_name == NULL || out_path == NULL || path_cap < 32) {
        errno = EINVAL;
        return -1;
    }
    if (daemon_device_validate_name(device_name) != 0) {
        return -1;
    }

    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        if (snprintf(out_path, path_cap, "%s/waddle/devices/%s", xdg, device_name) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        const char *home = getenv("HOME");
        if (home == NULL) home = "/root";
        if (snprintf(out_path, path_cap, "%s/.local/state/waddle/devices/%s", home, device_name) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    return recursive_mkdir(out_path);
}

int daemon_device_get_socket_path(const char *device_name, char *out_path, size_t path_cap) {
    if (out_path == NULL || path_cap < 32) {
        errno = EINVAL;
        return -1;
    }

    char runtime_dir[WaddleMaxPathLen];
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/waddle", xdg);
    } else {
        snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/waddle-%u", (unsigned)getuid());
    }

    if (device_name == NULL || device_name[0] == '\0') {
        if (snprintf(out_path, path_cap, "%s/daemon.sock", runtime_dir) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (snprintf(out_path, path_cap, "%s/%s/daemon.sock", runtime_dir, device_name) >= (int)path_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        /* Ensure device runtime directory exists */
        char dev_runtime[WaddleMaxPathLen + 128];
        snprintf(dev_runtime, sizeof(dev_runtime), "%.800s/%.64s", runtime_dir, device_name);
        (void)recursive_mkdir(dev_runtime);
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
    if (list == NULL || list->count == 0) {
        return BaseVsockCid;
    }

    uint32_t highest = BaseVsockCid - 1;
    for (size_t i = 0; i < list->count; i++) {
        if (list->devices[i].vsock_cid > highest) {
            highest = list->devices[i].vsock_cid;
        }
    }
    return highest + 1;
}

/**
 * @brief Checks whether a daemon supervisor is actively responding on a given socket.
 *
 * @param[in] socket_path Path to UNIX domain socket.
 * @return 1 if responding, 0 otherwise.
 */
static int is_socket_active(const char *socket_path) {
    if (socket_path == NULL || socket_path[0] == '\0') {
        return 0;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return (r == 0) ? 1 : 0;
}

int daemon_device_list(device_list_t *list) {
    if (list == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(list, 0, sizeof(*list));

    char config_dir[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(config_dir, sizeof(config_dir)) != 0) {
        return -1;
    }

    DIR *dir = opendir(config_dir);
    if (dir == NULL) {
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        size_t name_len = strlen(ent->d_name);
        if (name_len <= 4 || strcmp(ent->d_name + name_len - 4, ".ini") != 0) {
            continue;
        }

        if (list->count >= MaxDeviceCount) {
            break;
        }

        device_info_t *info = &list->devices[list->count];
        memset(info, 0, sizeof(*info));

        size_t dev_name_len = name_len - 4;
        if (dev_name_len >= WaddleMaxDeviceNameLen) {
            dev_name_len = WaddleMaxDeviceNameLen - 1;
        }
        memcpy(info->name, ent->d_name, dev_name_len);
        info->name[dev_name_len] = '\0';

        snprintf(info->config_path, sizeof(info->config_path), "%.800s/%.200s", config_dir, ent->d_name);
        daemon_device_get_state_dir(info->name, info->state_dir, sizeof(info->state_dir));
        daemon_device_get_socket_path(info->name, info->socket_path, sizeof(info->socket_path));

        /* Load device config values */
        daemon_config_t cfg;
        daemon_config_init_defaults(&cfg);
        if (daemon_config_load_file(&cfg, info->config_path) == 0) {
            snprintf(info->disk_image, sizeof(info->disk_image), "%s", cfg.disk_image);
            info->vsock_cid = cfg.vsock_cid;
            info->vsock_port = cfg.vsock_port;
            info->memory_mb = cfg.memory_mb;
            info->vcpus = cfg.vcpus;
        }

        info->is_running = is_socket_active(info->socket_path);
        list->count++;
    }

    closedir(dir);
    return (int)list->count;
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
    if (daemon_device_validate_name(name) != 0) {
        return -1;
    }

    /* Check if device already exists */
    device_info_t existing;
    if (daemon_device_find(name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }

    char config_dir[WaddleMaxPathLen];
    if (daemon_device_get_config_dir(config_dir, sizeof(config_dir)) != 0) {
        return -1;
    }

    char state_dir[WaddleMaxPathLen];
    if (daemon_device_get_state_dir(name, state_dir, sizeof(state_dir)) != 0) {
        return -1;
    }

    /* Determine base disk */
    char base_disk[WaddleMaxPathLen];
    int has_base = 0;
    if (custom_base_disk != NULL && custom_base_disk[0] != '\0') {
        if (access(custom_base_disk, R_OK) == 0) {
            snprintf(base_disk, sizeof(base_disk), "%s", custom_base_disk);
            has_base = 1;
        }
    }
    if (!has_base) {
        if (daemon_device_find_base_disk(base_disk, sizeof(base_disk)) == 0) {
            has_base = 1;
        }
    }

    /* Allocate disk path */
    char disk_path[WaddleMaxPathLen + 64];
    snprintf(disk_path, sizeof(disk_path), "%.900s/disk.qcow2", state_dir);

    /* Create QCOW2 overlay disk if base exists, or standalone QCOW2 */
    if (access(disk_path, F_OK) != 0) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Redirect stdout/stderr to /dev/null */
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            if (has_base) {
                execlp("qemu-img", "qemu-img", "create", "-f", "qcow2",
                       "-b", base_disk, "-F", "qcow2", disk_path, (char *)NULL);
            } else {
                execlp("qemu-img", "qemu-img", "create", "-f", "qcow2",
                       disk_path, "64G", (char *)NULL);
            }
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                /* Fallback if qemu-img fails: touch disk file */
                int fd = open(disk_path, O_CREAT | O_WRONLY, 0644);
                if (fd >= 0) close(fd);
            }
        }
    }

    /* Query existing devices to allocate a unique CID */
    device_list_t list;
    daemon_device_list(&list);
    uint32_t cid = daemon_device_allocate_cid(&list);

    /* Generate configuration file */
    char config_path[WaddleMaxPathLen + 64];
    snprintf(config_path, sizeof(config_path), "%.900s/%.64s.ini", config_dir, name);

    const char *home = getenv("HOME");
    if (home == NULL) home = "/home/dev";

    FILE *f = fopen(config_path, "w");
    if (f == NULL) {
        return -1;
    }

    fprintf(f, "# Waddle Subsystem Configuration: %s\n", name);
    fprintf(f, "[subsystem]\n");
    fprintf(f, "name = %s\n", name);
    fprintf(f, "memory_mb = %u\n", (unsigned)DefaultDeviceMemoryMb);
    fprintf(f, "vcpus = %u\n", (unsigned)DefaultDeviceVcpus);
    fprintf(f, "disk_image = %s\n", disk_path);
    fprintf(f, "vsock_cid = %u\n", (unsigned)cid);
    fprintf(f, "vsock_port = %u\n", (unsigned)WaddleDefaultVsockPortVal);
    fprintf(f, "default_shell = powershell.exe\n\n");

    fprintf(f, "[filesystem]\n");
    fprintf(f, "mount = %s:Z:\\:rw\n\n", home);

    fprintf(f, "[timeouts]\n");
    fprintf(f, "start_timeout = %u\n", (unsigned)ConfigDefaultStartTimeoutSec);
    fprintf(f, "stop_timeout = %u\n", (unsigned)ConfigDefaultStopTimeoutSec);

    fclose(f);

    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
        snprintf(out_info->name, sizeof(out_info->name), "%.63s", name);
        snprintf(out_info->config_path, sizeof(out_info->config_path), "%.1023s", config_path);
        snprintf(out_info->state_dir, sizeof(out_info->state_dir), "%.1023s", state_dir);
        snprintf(out_info->disk_image, sizeof(out_info->disk_image), "%.1023s", disk_path);
        daemon_device_get_socket_path(name, out_info->socket_path, sizeof(out_info->socket_path));
        out_info->vsock_cid = cid;
        out_info->vsock_port = WaddleDefaultVsockPortVal;
        out_info->memory_mb = DefaultDeviceMemoryMb;
        out_info->vcpus = DefaultDeviceVcpus;
        out_info->is_running = 0;
    }

    return 0;
}
