#ifndef WaddleAvDmabufH
#define WaddleAvDmabufH
#include <stdint.h>
/** @brief Export a page-aligned KVMFR pixel region to a real DMA-BUF.
 * @param[in] device_fd Borrowed open KVMFR FD; regular files are rejected by ioctl.
 * @param[in] offset Page-aligned byte offset into KVMFR mapping.
 * @param[in] size Nonzero page-aligned accessible region bytes.
 * @return Owned CLOEXEC DMA-BUF FD, or -1 with errno from validation/ioctl.
 * @note Thread-safe for independent regions; caller closes successful FD after
 * import submission, compositor retains its duplicate. No userspace allocation.
 */
int av_dmabuf_export(int device_fd, uint64_t offset, uint64_t size);
#endif
