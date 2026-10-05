#ifndef WaddleVenusBoundsH
#define WaddleVenusBoundsH
#include <stddef.h>
#include <stdint.h>

/** @brief Validate a borrowed byte mapping using Zig bounds-checked slices.
 * @param[in] mapping Nonnull actual mapping, length bytes accessible.
 * @param[in] length Accessible bytes; no alignment requirement in this helper.
 * @return Valid capacity or zero for invalid metadata, padding, or extent.
 * @note Pure, no allocation; caller guarantees immutable metadata during attach.
 */
uint32_t venus_bounds_capacity(const uint8_t *mapping, size_t length);
/** @brief Copy into a ring with at most two bounded spans.
 * @param[out] payload Borrowed nonnull capacity-byte ring payload.
 * @param[in] capacity Validated power of two, 64..16777216 bytes.
 * @param[in] cursor Producer cursor, modulo 2^32.
 * @param[in] source Nonnull borrowed length-byte source, disjoint from payload.
 * @param[in] length Validated bytes, 1..capacity, exclusively owned free space.
 * @note Producer-only; no allocation, cursor changes, or publication.
 */
void venus_bounds_write(uint8_t *payload, uint32_t capacity, uint32_t cursor,
                        const uint8_t *source, size_t length);
/** @brief Copy out of a ring with at most two bounded spans.
 * @param[in] payload Borrowed nonnull capacity-byte ring payload.
 * @param[in] capacity Validated power of two, 64..16777216 bytes.
 * @param[in] cursor Consumer cursor, modulo 2^32.
 * @param[out] destination Nonnull borrowed length-byte output, disjoint from payload.
 * @param[in] length Validated bytes, 1..capacity, published available data.
 * @note Consumer-only; no allocation, cursor changes, or publication.
 */
void venus_bounds_read(const uint8_t *payload, uint32_t capacity, uint32_t cursor,
                       uint8_t *destination, size_t length);
/** @brief Check region extent and two-ring layout before initialization.
 * @param[in] length Proposed actual bytes, power of two, 4096..one GiB.
 * @param[in] capacity Proposed bounded power-of-two ring bytes.
 * @return One when valid, zero otherwise; pure/thread-safe, no allocation.
 */
int venus_bounds_region_size(size_t length, uint32_t capacity);
/** @brief Validate immutable region byte metadata before pointer calculations.
 * @param[in] mapping Nonnull borrowed length-byte region, metadata immutable.
 * @param[in] length Actual accessible bytes, untrusted headers cannot extend it.
 * @param[out] region_bytes Nonnull private extent output, zeroed on failure.
 * @return Valid ring capacity or zero; pure/thread-safe, no allocation/mutation.
 */
uint32_t venus_bounds_region_capacity(const uint8_t *mapping, size_t length, uint64_t *region_bytes);
#endif
