/** @file venus_receiver_bounds.h @brief Internal bounded C ABI into Zig. */
#ifndef WaddleVenusReceiverBoundsH
/** @brief Compile-time include guard; no storage or lifetime. */
#define WaddleVenusReceiverBoundsH
#include <stddef.h>
#include <stdint.h>
/** @brief Validate receiver buffer extents before allocation.
 * @param[in] capacity Command scratch bytes. @param[in] reply_bytes Reply extent.
 * @return One for valid power-of-two ranges, zero otherwise.
 * @note Pure/thread-safe, no allocation or ownership transfer.
 */
int venus_receiver_limits(uint32_t capacity, uint64_t reply_bytes);
/** @brief Bounds-check and copy one immutable command bundle into private scratch.
 * @param[out] output Nonnull capacity-byte private scratch, disjoint from input.
 * @param[in] capacity Validated actual scratch extent.
 * @param[in] input Nonnull immutable borrowed input[length].
 * @param[in] length Multiple of four, 8..capacity.
 * @return RingOk (zero) or RingInvalid (-1); error leaves output unchanged.
 * @note Allocation-free/thread-safe for disjoint buffers; does not decode Venus.
 */
int venus_receiver_command_copy(void *output, uint32_t capacity, const void *input, size_t length);
/** @brief Bounds-check a reply range before forming any source offset pointer.
 * @param[out] output Nonnull private output[length], disjoint from reply mapping.
 * @param[in] input Nonnull borrowed actual extent-byte reply mapping, completed.
 * @param[in] extent Validated mapped reply extent, at most 16777216.
 * @param[in] offset Proposed reply offset. @param[in] length Nonzero bytes to copy.
 * @return RingOk or RingInvalid; errors preserve output.
 * @note Allocation-free/thread-safe on disjoint outputs; caller synchronizes input.
 */
int venus_receiver_reply_copy(void *output, const void *input, uint64_t extent, uint64_t offset,
                              size_t length);
#endif
