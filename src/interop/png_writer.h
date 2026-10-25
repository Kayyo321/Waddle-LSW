/** @file png_writer.h @brief Bounded caller-buffer PNG encoder for icon RGBA pixels. */
#ifndef PngWriterH
#define PngWriterH

#include <stddef.h>
#include <stdint.h>

/** @brief Smallest supported square icon dimension, in pixels. */
#define PngWriterMinDimension 16u
/** @brief Largest supported square icon dimension, in pixels. */
#define PngWriterMaxDimension 256u
/** @brief Exact count of supported frozen icon dimensions. */
#define PngWriterSupportedDimensionCount 7u

/** @brief Result of a bounded PNG writer operation. */
typedef enum png_writer_status_t {
    PngWriterStatusOk = 0, /**< Exact PNG output was written. */
    PngWriterStatusInvalidArgument, /**< Pointer, alignment, stride, overlap, or range is invalid. */
    PngWriterStatusUnsupportedDimension, /**< Dimension is not in the frozen size set. */
    PngWriterStatusOverflow, /**< A caller-provided stride extent overflowed size_t. */
    PngWriterStatusInsufficientCapacity /**< Output is smaller than the exact required PNG size. */
} png_writer_status_t;

/**
 * @brief Return the exact encoded byte count for a frozen square icon size.
 * @param[in] dimension Square pixel width and height; only 16, 24, 32, 48,
 *        64, 128, and 256 are supported.
 * @param[out] size_out Naturally aligned writable output, disjoint from all
 *        inputs; unchanged on failure.
 * @return PngWriterStatusOk, UnsupportedDimension, InvalidArgument, or Overflow.
 * @note No allocation or shared state. Thread-safe for independent buffers.
 */
png_writer_status_t png_writer_encoded_size(uint32_t dimension, size_t *size_out);

/**
 * @brief Encode top-down straight-alpha RGBA8 pixels to a minimal PNG.
 * @param[in] rgba Borrowed readable pixels in row-major RGBA byte order.
 *        Required readable extent is `(dimension - 1) * stride + dimension * 4`.
 * @param[in] stride Bytes between source rows; at least `dimension * 4`.
 * @param[in] dimension Square image size from the frozen supported set.
 * @param[out] output Caller-owned writable storage with capacity at least the
 *        exact result from png_writer_encoded_size; disjoint from input.
 * @param[in] output_capacity Writable extent of output.
 * @param[out] written_out Naturally aligned output disjoint from both buffers;
 *        unchanged on failure.
 * @return PngWriterStatusOk, InvalidArgument, UnsupportedDimension, Overflow,
 *         or InsufficientCapacity.
 * @note Emits non-interlaced 8-bit RGBA PNG using stored deflate blocks. No
 *       allocation, input mutation, premultiplication, or pointer retention.
 *       Caller owns output and serializes access to overlapping buffers.
 */
png_writer_status_t png_writer_encode_rgba8(const uint8_t *rgba,
                                             size_t stride,
                                             uint32_t dimension,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             size_t *written_out);

#endif
