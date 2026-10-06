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
 * @param[in] extent Validated mapped reply extent, at most one GiB.
 * @param[in] offset Proposed reply offset. @param[in] length Nonzero bytes to copy.
 * @return RingOk or RingInvalid; errors preserve output.
 * @note Allocation-free/thread-safe on disjoint outputs; caller synchronizes input.
 */
int venus_receiver_reply_copy(void *output, const void *input, uint64_t extent, uint64_t offset,
                              size_t length);

/** @brief Validate additional storage policy before ledger/native access.
 * @param[in] id Requested ID. @param[in] blob Blob zero or device-memory identity.
 * @param[in] bytes Requested declared extent. @param[in] flags Requested storage flags.
 * @return One for valid ID/size/flags, zero otherwise; pure/no allocation/thread-safe.
 */
int venus_receiver_resource_request(uint32_t id, uint64_t blob, uint64_t bytes, uint32_t flags);
/** @brief Convert a bounded ID into a one-based ledger slot.
 * @param[in] id Proposed resource ID, untrusted host value.
 * @return 1..64 for IDs 2..65, zero otherwise; pure/no allocation/thread-safe.
 */
uint32_t venus_receiver_resource_slot(uint32_t id);
/** @brief Validate a nonempty resource range without pointer arithmetic.
 * @param[in] extent Validated actual bytes. @param[in] offset Requested offset.
 * @param[in] length Requested private buffer bytes.
 * @return One when valid, zero otherwise; pure/no allocation/thread-safe.
 */
int venus_receiver_resource_range(uint64_t extent, uint64_t offset, size_t length);
/** @brief Copy private input into a bounded CPU resource range.
 * @param[out] memory Nonnull actual extent-byte mapped CPU resource.
 * @param[in] extent Validated bytes. @param[in] offset Requested offset.
 * @param[in] input Nonnull immutable disjoint input[length].
 * @param[in] length Nonzero bounded bytes.
 * @return RingOk or RingInvalid; errors do not modify memory.
 * @note Pure, allocation-free/thread-safe for disjoint ranges; caller synchronizes map.
 */
int venus_receiver_memory_write(void *memory, uint64_t extent, uint64_t offset, const void *input,
                                size_t length);
#endif
