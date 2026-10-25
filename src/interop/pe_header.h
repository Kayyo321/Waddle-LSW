#ifndef WaddleInteropPeHeaderH
#define WaddleInteropPeHeaderH
/** @file pe_header.h @brief Bounded allocation-free PE routing classification. */
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum readable prefix; no call reads beyond this bound. */
#define InteropPePrefixBytes 4096u
/** @brief Routing hint only; neither value proves executable validity. */
typedef enum interop_pe_kind_t {
    InteropPeGui = 0, /**< GUI fallback for malformed, unknown or GUI input. */
    InteropPeConsole = 1 /**< PE32/PE32+ bounded Subsystem3. */
} interop_pe_kind_t;
/** @brief ABI result; malformed file contents are successful GUI fallback. */
typedef enum interop_pe_status_t {
    InteropPeSuccess = 0, /**< Kind was published. */
    InteropPeInvalidArgument = 1 /**< Invalid pointer/range/alias/length; output unchanged. */
} interop_pe_status_t;
/**
 * @brief Classify a private, already-read file prefix without allocating.
 * @param[in] bytes Borrowed stable readable bytes, NULL permitted only if length0.
 * @param[in] length Number of readable bytes,0..InteropPePrefixBytes.
 * @param[out] kind_out Nonnull aligned writable uint32_t disjoint from input.
 * @return InteropPeSuccess with an interop_pe_kind_t integer, or
 *         InteropPeInvalidArgument with output unchanged.
 * @note No pointers retained or ownership transferred. Pure and thread-safe
 *       for independent storage. Read failures must be handled by the caller,
 *       separately from GUI fallback; never launch based on invalid API calls.
 */
int interop_pe_classify_prefix(const uint8_t *bytes, size_t length, uint32_t *kind_out);
#endif
