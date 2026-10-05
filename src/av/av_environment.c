#include "av_environment.h"
#include "av_gpu.h"
#include "av_layout.h"
#include "av_kvmfr.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
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
    struct stat owned, current;
    int matches = environment->created_file && environment->fd >= 0 &&
                  fstat(environment->fd, &owned) == 0 && lstat(environment->path, &current) == 0 &&
                  owned.st_dev == current.st_dev && owned.st_ino == current.st_ino;
    if (environment->mapping) {
        munmap(environment->mapping, environment->length);
        environment->mapping = NULL;
    }
    if (matches)
        unlink(environment->path);
    if (environment->fd >= 0) {
        close(environment->fd);
        environment->fd = -1;
    }
    environment->created_file = 0;
    environment->length = 0;
    environment->path[0] = '\0';
}
/* Persistent firmware belongs to the device disk, not a capture session. */
static int prepare_firmware(const daemon_config_t *config) {
    if (!config->av_uefi) return 0;
    if (strchr(config->disk_image, ',') || strchr(config->disk_image, '\n')) {
        errno = EINVAL;
        return -1;
    }
    if (access("/usr/share/OVMF/OVMF_CODE_4M.fd", R_OK) != 0) return -1;
    int input = open("/usr/share/OVMF/OVMF_VARS_4M.fd", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return -1;
    char destination[WaddleMaxPathLen + 16], temporary[WaddleMaxPathLen + 32] = {0};
    struct stat source, existing;
    int output = -1, result = -1, created = 0;
    if (fstat(input, &source) != 0 || !S_ISREG(source.st_mode) || source.st_size <= 0 ||
        source.st_size > 16777216 || snprintf(destination, sizeof(destination), "%s.av_uefi.fd", config->disk_image) >= (int)sizeof(destination)) goto cleanup;
    output = open(destination, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (output >= 0 || errno != ENOENT) {
        if (output >= 0 && fstat(output, &existing) == 0 && S_ISREG(existing.st_mode) &&
            existing.st_uid == geteuid() && !(existing.st_mode & 0077) && existing.st_size == source.st_size)
            result = 0;
        else errno = EINVAL;
        goto cleanup;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", destination) >= (int)sizeof(temporary)) goto cleanup;
    output = mkostemp(temporary, O_CLOEXEC);
    if (output < 0) goto cleanup;
    created = 1;
    char bytes[65536];
    for (;;) {
        ssize_t count = read(input, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto cleanup;
        if (!count) break;
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(output, bytes + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto cleanup;
            offset += written;
        }
    }
    if (fsync(output) == 0 && link(temporary, destination) == 0) result = 0;
cleanup:
    {
        int saved = errno;
        close(input);
        if (output >= 0) close(output);
        if (created) unlink(temporary);
        errno = saved;
    }
    return result;
}
int av_environment_prepare(av_environment_t *environment, const daemon_config_t *config,
                           char *error, size_t capacity) {
    if (!capacity) {
        errno = EINVAL;
        return -1;
    }
    error[0] = '\0';
    if (environment->fd >= 0 || environment->mapping) {
        errno = EBUSY;
        snprintf(error, capacity, "AV resource already active");
        return -1;
    }
    if (daemon_config_validate(config) != 0 || !config->av_enabled) {
        errno = EINVAL;
        return fail(environment, error, capacity, "configuration");
    }
    if (prepare_firmware(config) != 0)
        return fail(environment, error, capacity, "persistent OVMF firmware preparation");
    if (config->av_gpu_bdf[0] || config->av_gpu_mdev_uuid[0]) {
        if (av_gpu_mdev_probe("/sys", config->av_gpu_bdf, config->av_gpu_mdev_uuid,
                              error, capacity) != 0) return -1;
        if (!config->av_gpu_mdev_uuid[0]) {
            errno = EINVAL;
            snprintf(error, capacity, "AV GPU slicing requires gpu_mdev_uuid; physical GPU preserved");
            return -1;
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
    /* The daemon retains this lease across QEMU and audio/video workers. Slot
     * states alone cannot identify an active audio-only or idle session. */
    if (is_device && flock(environment->fd, LOCK_EX | LOCK_NB) != 0)
        return fail(environment, error, capacity, "KVMFR runtime ownership lease");
    struct stat status;
    if (fstat(environment->fd, &status) != 0)
        return fail(environment, error, capacity, "mapping stat");
    if (is_device) {
        long bytes = av_kvmfr_size(environment->fd);
        if (!S_ISCHR(status.st_mode) || bytes < 0 || (uint64_t)bytes != AvMappingBytes ||
            lseek(environment->fd, 0, SEEK_END) != (off_t)AvMappingBytes) {
            if (bytes >= 0)
                errno = EINVAL;
            return fail(environment, error, capacity, "KVMFR exact-size probe");
        }
    } else if (!S_ISREG(status.st_mode)) {
        errno = EINVAL;
        return fail(environment, error, capacity, "shared-memory file type");
    } else if (ftruncate(environment->fd, AvMappingBytes) != 0) {
        return fail(environment, error, capacity, "shared-memory sizing");
    }
    environment->length = AvMappingBytes;
    void *mapping =
        mmap(NULL, environment->length, PROT_READ | PROT_WRITE, MAP_SHARED, environment->fd, 0);
    if (mapping == MAP_FAILED)
        return fail(environment, error, capacity, "shared-memory mmap");
    environment->mapping = mapping;
    if (is_device) {
        uint32_t magic;
        memcpy(&magic, mapping, sizeof(magic));
        if (magic != 0 && av_layout_validate(mapping, environment->length) != 0) {
            errno = EBUSY;
            return fail(environment, error, capacity, "KVMFR already contains another mapping ABI");
        }
        if (magic != 0) {
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
