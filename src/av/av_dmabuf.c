#include "av_dmabuf.h"
#include "kvmfr.h"
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
int av_dmabuf_export(int device_fd, uint64_t offset, uint64_t size) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !size || offset % (uint64_t)page_size || size % (uint64_t)page_size) {
        errno = EINVAL;
        return -1;
    }
    long capacity = ioctl(device_fd, KVMFR_DMABUF_GETSIZE);
    if (capacity < 0)
        return -1;
    if (offset > (uint64_t)capacity || size > (uint64_t)capacity - offset) {
        errno = EINVAL;
        return -1;
    }
    struct kvmfr_dmabuf_create request = {
        .flags = KVMFR_DMABUF_FLAG_CLOEXEC, .offset = offset, .size = size};
    return ioctl(device_fd, KVMFR_DMABUF_CREATE, &request);
}
