#ifndef WaddleAvKvmfrH
#define WaddleAvKvmfrH
#include "kvmfr.h"
#include <stdint.h>
#include <sys/ioctl.h>
/** @brief Owned patched-driver capacity query; eight-byte out parameter, native order. */
#define AvKvmfrGetSize64 _IOR('u', 0x45, uint64_t)
/** @brief Probe patched KVMFR capacity without truncating the legacy ioctl return.
 * @param[in] fd Borrowed descriptor; remains caller-owned, not closed.
 * @return Nonnegative capacity bytes, or -1 with kernel errno.
 * @note Thread-safe, allocation-free; Linux 64-bit host required. Both libc and
 * the kernel ioctl dispatch truncate the legacy positive size to int. This
 * extension copies uint64 capacity to userspace and returns ordinary 0/-errno.
 * Unpatched drivers fail ENOTTY and are preserved for administrator review.
 */
static inline long av_kvmfr_size(int fd) {
    _Static_assert(sizeof(long) >= 8, "AV KVMFR requires a 64-bit Linux host");
    uint64_t capacity = 0;
    if (ioctl(fd, AvKvmfrGetSize64, &capacity) != 0)
        return -1;
    return capacity <= INT64_MAX ? (long)capacity : -1;
}
#endif
