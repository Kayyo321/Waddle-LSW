#ifndef WaddleAvKvmfrH
#define WaddleAvKvmfrH
#include "kvmfr.h"
#include <sys/syscall.h>
#include <unistd.h>
/** @brief Probe KVMFR capacity without truncating the driver's long return value.
 * @param[in] fd Borrowed descriptor; remains caller-owned, not closed.
 * @return Nonnegative capacity bytes, or -1 with kernel errno.
 * @note Thread-safe, allocation-free; Linux 64-bit host required. libc ioctl()
 * returns int and truncates a valid 2 GiB size; SYS_ioctl preserves kernel long.
 */
static inline long av_kvmfr_size(int fd) {
    _Static_assert(sizeof(long) >= 8, "AV KVMFR requires a 64-bit Linux host");
    return syscall(SYS_ioctl, fd, KVMFR_DMABUF_GETSIZE, 0);
}
#endif
