#ifndef WaddleAvGpuH
#define WaddleAvGpuH
#include <stddef.h>
/** @brief Read-only PCI/IOMMU capability probe; never binds or detaches devices.
 * @param[in] sysfs Nonnull trusted sysfs root, normally /sys; borrowed during call.
 * @param[in] bdf Nonnull NUL-terminated PCI address, validated before path use.
 * @param[out] error Nonnull caller-owned diagnostic[capacity].
 * @param[in] capacity Nonzero accessible diagnostic bytes.
 * @return 0 all group members unbound/vfio-pci, -1 malformed/missing/busy group.
 * @note Thread-safe, no retained pointers; closes directory and allocates no heap
 * beyond libc directory state. errno distinguishes EINVAL, EBUSY and I/O errors.
 * This snapshot does not reserve hardware; QEMU still verifies VFIO ownership.
 */
int av_gpu_probe(const char *sysfs, const char *bdf, char *error, size_t capacity);
/** @brief Validate bounded PCI address before filesystem access.
 * @param[in] bytes Nonnull borrowed input[length].
 * @param[in] length Accessible bytes excluding NUL.
 * @return 0 valid domain:bus:slot.function, -1 invalid. Pure/thread-safe, no allocation.
 */
int av_gpu_bdf_validate(const char *bytes, size_t length);
#endif
