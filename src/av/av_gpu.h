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
/** @brief Validate bounded canonical lowercase mdev UUID syntax.
 * @param[in] bytes Nonnull borrowed bytes[length], excluding NUL.
 * @param[in] length Accessible input byte count.
 * @return 0 valid, -1 invalid; pure/thread-safe, no allocation or retained data. */
int av_gpu_uuid_validate(const char *bytes, size_t length);
/** @brief Read-only mdev capability/identity snapshot; host drivers untouched.
 * @param[in] sysfs Nonnull trusted sysfs root, borrowed during call.
 * @param[in] bdf Nonnull optional parent BDF, empty discovers parent from UUID.
 * @param[in] uuid Nonnull canonical UUID, empty performs parent capability probe.
 * @param[out] error Nonnull caller-owned diagnostic[capacity].
 * @param[in] capacity Nonzero writable diagnostic capacity.
 * @return 0 supported parent/existing PCI mdev, -1 invalid/unsupported/missing;
 * errno is EINVAL, ENOTSUP or filesystem error. Does not reserve/create a slice.
 * @note Thread-safe; stack buffers borrowed only during call; closes every opened
 * directory/file on all paths. No persistent allocation, device or driver writes. */
int av_gpu_mdev_probe(const char *sysfs, const char *bdf, const char *uuid,
                      char *error, size_t capacity);
#endif
