#include "av_environment.h"
#include "av_layout.h"
#include "kvmfr.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(av_environment_t *environment, char *error, size_t capacity,
                const char *operation) {
    int saved = errno;
    snprintf(error, capacity, "AV %s: %s (%d)", operation, strerror(saved), saved);
    av_environment_free(environment);
    errno = saved;
    return -1;
}
void av_environment_free(av_environment_t *environment) {
    if (environment->mapping) {
        munmap(environment->mapping, environment->length);
        environment->mapping = NULL;
    }
    if (environment->fd >= 0) {
        close(environment->fd);
        environment->fd = -1;
    }
    if (environment->created_file) {
        unlink(environment->path);
        environment->created_file = 0;
    }
    environment->length = 0;
    environment->path[0] = '\0';
}
int av_environment_prepare(av_environment_t *environment, const daemon_config_t *config,
                           char *error, size_t capacity) {
    if (!capacity) {
        errno = EINVAL;
        return -1;
    }
    error[0] = '\0';
    if (daemon_config_validate(config) != 0 || !config->av_enabled || environment->fd >= 0 ||
        environment->mapping) {
        errno = EINVAL;
        return fail(environment, error, capacity, "configuration");
    }
    if (config->av_gpu_bdf[0]) {
        char driver_path[128], driver[256];
        snprintf(driver_path, sizeof(driver_path), "/sys/bus/pci/devices/%s/driver",
                 config->av_gpu_bdf);
        ssize_t count = readlink(driver_path, driver, sizeof(driver) - 1);
        if (count < 0)
            return fail(environment, error, capacity, "GPU driver probe");
        driver[count] = '\0';
        char *name = strrchr(driver, '/');
        if (!name || strcmp(name + 1, "vfio-pci")) {
            errno = EBUSY;
            return fail(environment, error, capacity,
                        "GPU must be explicitly assigned to vfio-pci");
        }
    }
    snprintf(environment->path, sizeof(environment->path), "%s", config->av_shm_path);
    int is_device = strncmp(config->av_shm_path, "/dev/kvmfr", 10) == 0;
    if (is_device) {
        environment->fd = open(config->av_shm_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    } else {
        environment->fd =
            open(config->av_shm_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_EXCL, 0600);
        if (environment->fd >= 0)
            environment->created_file = 1;
    }
    if (environment->fd < 0)
        return fail(environment, error, capacity,
                    "shared-memory open (existing files are preserved)");
    struct stat status;
    if (fstat(environment->fd, &status) != 0)
        return fail(environment, error, capacity, "mapping stat");
    if (is_device) {
        long bytes = ioctl(environment->fd, KVMFR_DMABUF_GETSIZE);
        if (!S_ISCHR(status.st_mode) || bytes < 0 || (uint64_t)bytes != AvMappingBytes) {
            if (bytes >= 0)
                errno = EINVAL;
            return fail(environment, error, capacity, "KVMFR exact-size probe");
        }
    } else if (!S_ISREG(status.st_mode) || ftruncate(environment->fd, AvMappingBytes) != 0) {
        return fail(environment, error, capacity, "shared-memory sizing");
    }
    environment->length = AvMappingBytes;
    void *mapping =
        mmap(NULL, environment->length, PROT_READ | PROT_WRITE, MAP_SHARED, environment->fd, 0);
    if (mapping == MAP_FAILED)
        return fail(environment, error, capacity, "shared-memory mmap");
    environment->mapping = mapping;
    if (is_device) {
        if (av_layout_validate(mapping, environment->length) == 0) {
            for (unsigned pool = 0; pool < AvMaxWindows; ++pool)
                for (unsigned index = 0; index < AvVideoBuffers; ++index) {
                    window_slot_header_t *slot =
                        av_layout_slot(mapping, environment->length, pool, index);
                    if (atomic_load_explicit(&slot->slot_state, memory_order_acquire) != SlotFree) {
                        errno = EBUSY;
                        return fail(environment, error, capacity,
                                    "KVMFR has outstanding slot ownership");
                    }
                }
        }
    }
    if (av_layout_init(mapping, environment->length) != 0) {
        errno = EINVAL;
        return fail(environment, error, capacity, "shared-memory initialization");
    }
    return 0;
}
