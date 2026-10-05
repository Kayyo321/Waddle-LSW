/** @file av_gpu.c @brief Fail-closed read-only IOMMU group eligibility checks. */
#include "av_gpu.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int reject(char *error, size_t capacity, const char *bdf, const char *reason) {
    int saved = errno;
    snprintf(error, capacity, "GPU %s: %s (%s); host drivers preserved", bdf, reason, strerror(saved));
    errno = saved;
    return -1;
}
int av_gpu_probe(const char *sysfs, const char *bdf, char *error, size_t capacity) {
    if (!capacity) { errno = EINVAL; return -1; }
    error[0] = '\0';
    if (av_gpu_bdf_validate(bdf, strlen(bdf)) != 0) {
        errno = EINVAL; return reject(error, capacity, bdf, "invalid PCI address");
    }
    char path[PATH_MAX], group[PATH_MAX], groups[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/kernel/iommu_groups", sysfs) >= (int)sizeof(path) ||
        !realpath(path, groups)) return reject(error, capacity, bdf, "IOMMU groups unavailable");
    if (snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/iommu_group", sysfs, bdf) >= (int)sizeof(path) ||
        !realpath(path, group)) return reject(error, capacity, bdf, "IOMMU group unavailable");
    size_t prefix = strlen(groups);
    if (strncmp(group, groups, prefix) || group[prefix] != '/' ||
        !group[prefix + 1] || strchr(group + prefix + 1, '/')) {
        errno = EINVAL; return reject(error, capacity, bdf, "group escapes sysfs IOMMU root");
    }
    if (snprintf(path, sizeof(path), "%s/devices", group) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return reject(error, capacity, bdf, "group path too long");
    }
    DIR *directory = opendir(path);
    if (!directory) return reject(error, capacity, bdf, "group devices unavailable");
    int result = 0, found = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = reject(error, capacity, bdf, "group read failed"); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (av_gpu_bdf_validate(entry->d_name, strlen(entry->d_name)) != 0) {
            errno = EINVAL; result = reject(error, capacity, bdf, "invalid group member"); break;
        }
        found |= !strcmp(entry->d_name, bdf);
        if (snprintf(path, sizeof(path), "%s/devices/%s", group, entry->d_name) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG; result = reject(error, capacity, bdf, "member path too long"); break;
        }
        struct stat status;
        if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
            errno = ENODEV; result = reject(error, capacity, bdf, "missing group member"); break;
        }
        size_t length = strlen(path);
        if (length + 8 > sizeof(path)) { errno = ENAMETOOLONG; result = -1; break; }
        memcpy(path + length, "/driver", 8);
        char driver[PATH_MAX];
        ssize_t count = readlink(path, driver, sizeof(driver) - 1);
        if (count < 0 && errno == ENOENT) continue;
        if (count < 0 || count == (ssize_t)sizeof(driver) - 1) {
            if (count >= 0) errno = EOVERFLOW;
            result = reject(error, capacity, entry->d_name, "driver link unreadable"); break;
        }
        driver[count] = '\0';
        const char *name = strrchr(driver, '/');
        if (!name || strcmp(name + 1, "vfio-pci")) {
            errno = EBUSY; result = reject(error, capacity, entry->d_name, driver); break;
        }
    }
    int saved = errno;
    closedir(directory);
    errno = saved;
    if (!result && !found) { errno = ENODEV; result = reject(error, capacity, bdf, "not in IOMMU group"); }
    return result;
}

/* Read one immutable API attribute; FILE ownership ends here on every path. */
static int pci_api(const char *type) {
    char path[PATH_MAX], value[32];
    if (snprintf(path, sizeof(path), "%s/device_api", type) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return -1;
    }
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    size_t count = fread(value, 1, sizeof(value) - 1, file);
    int failed = ferror(file);
    fclose(file);
    if (failed) { errno = EIO; return -1; }
    value[count] = '\0';
    if (strcmp(value, "vfio-pci\n") && strcmp(value, "vfio-pci")) {
        errno = ENOTSUP; return -1;
    }
    return 0;
}
static int supported_parent(const char *sysfs, const char *bdf, const char *type) {
    char path[PATH_MAX], supported[PATH_MAX], registered[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/mdev_supported_types", sysfs, bdf) >= (int)sizeof(path) ||
        !realpath(path, supported)) return -1;
    if (snprintf(path, sizeof(path), "%s/class/mdev_bus/%s/mdev_supported_types", sysfs, bdf) >= (int)sizeof(path) ||
        !realpath(path, registered)) return -1;
    if (strcmp(supported, registered)) { errno = EINVAL; return -1; }
    if (type) {
        size_t length = strlen(supported);
        if (strncmp(type, supported, length) || type[length] != '/' ||
            !type[length + 1] || strchr(type + length + 1, '/')) {
            errno = EINVAL; return -1;
        }
        return pci_api(type);
    }
    DIR *directory = opendir(supported);
    if (!directory) return -1;
    int result = -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) break;
        if (entry->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", supported, entry->d_name) >= (int)sizeof(path)) continue;
        if (pci_api(path) == 0) { result = 0; break; }
    }
    closedir(directory);
    if (result) errno = ENOTSUP;
    return result;
}
int av_gpu_mdev_probe(const char *sysfs, const char *bdf, const char *uuid,
                      char *error, size_t capacity) {
    if (!capacity) { errno = EINVAL; return -1; }
    error[0] = '\0';
    if ((bdf[0] && av_gpu_bdf_validate(bdf, strlen(bdf))) ||
        (uuid[0] && av_gpu_uuid_validate(uuid, strlen(uuid))) || (!bdf[0] && !uuid[0])) {
        errno = EINVAL; return reject(error, capacity, bdf, "invalid mdev UUID/parent");
    }
    char path[PATH_MAX], type[PATH_MAX];
    if (uuid[0]) {
        if (snprintf(path, sizeof(path), "%s/bus/mdev/devices/%s/mdev_type", sysfs, uuid) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG; return reject(error, capacity, uuid, "mdev path too long");
        }
        if (!realpath(path, type)) return reject(error, capacity, uuid, "existing mdev unavailable");
    }
    if (bdf[0]) {
        if (!supported_parent(sysfs, bdf, uuid[0] ? type : NULL)) return 0;
        if (!uuid[0]) errno = ENOTSUP;
        return reject(error, capacity, bdf,
            "GPU does not support the requested Mediated Devices (mdev) / vGPU slice. Host configuration/patching may be required");
    }
    if (snprintf(path, sizeof(path), "%s/class/mdev_bus", sysfs) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return reject(error, capacity, uuid, "mdev bus path too long");
    }
    DIR *directory = opendir(path);
    if (!directory) {
        errno = ENOTSUP; return reject(error, capacity, uuid, "GPU does not support Mediated Devices (mdev) / vGPU. Host configuration/patching may be required");
    }
    int result = -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) break;
        if (av_gpu_bdf_validate(entry->d_name, strlen(entry->d_name))) continue;
        if (!supported_parent(sysfs, entry->d_name, type)) { result = 0; break; }
    }
    closedir(directory);
    if (result) {
        errno = ENOTSUP; return reject(error, capacity, uuid, "mdev has no registered PCI vfio-pci parent/type");
    }
    return 0;
}
