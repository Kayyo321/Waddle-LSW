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
