/** @file venus_dmabuf.h @brief Private image layout and Wayland feedback validation. */
#ifndef WaddleVenusDmabufH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusDmabufH
#include "venus_ring.h"
/** @brief Maximum planes; all inactive entries must be zero. */
#define VenusDmabufMaxPlanes 4u
/** @brief Maximum feedback format table bytes, 4096 sixteen-byte entries. */
#define VenusDmabufMaxTableBytes 65536u
/** @brief Private allocation plane; host values, never cast onto a wire buffer.
 * @note Natural eight-byte alignment, size 24; no pointers/ownership. Caller
 * obtains size/extent from retained allocation and Vulkan layout queries.
 */
typedef struct venus_dmabuf_plane_t {
    uint32_t offset; /**< Byte offset from start of this plane's backing FD. */
    uint32_t stride; /**< Nonzero row stride, <= INT32_MAX bytes. */
    uint64_t size;   /**< Queried subresource layout size, excluding offset. */
    uint64_t extent; /**< Real allocation extent, 1..one GiB bytes. */
} venus_dmabuf_plane_t;
/** @brief Caller-owned immutable image geometry; no FD or pixel ownership.
 * @note Natural eight-byte alignment, size 120; private C/Zig ABI. Queries are
 * thread-safe on immutable records. Active plane count follows the DRM format.
 */
typedef struct venus_dmabuf_layout_t {
    uint32_t width;       /**< Width, 1..16384 pixels. */
    uint32_t height;      /**< Height, 1..16384 pixels. */
    uint32_t fourcc;      /**< DRM ARGB/XRGB/ABGR/XBGR8888 or NV12. */
    uint32_t plane_count; /**< One for packed formats, two for even-size NV12. */
    uint64_t modifier;    /**< Exact queried DRM modifier, also matched to feedback. */
    venus_dmabuf_plane_t planes[VenusDmabufMaxPlanes]; /**< Active then zero planes. */
} venus_dmabuf_layout_t;
/** @brief Validate image plane metadata before native Wayland requests.
 * @param[in] layout Nullable immutable private metadata, borrowed for call.
 * @return RingOk or RingInvalid for NULL/unsupported geometry/out-of-bounds layout.
 * @note Pure, allocation-free, thread-safe. Nonlinear modifiers require driver-
 * queried size; validation cannot prove guest-supplied capacities or FD identity.
 * Does not map pixels, authorize GPU reuse, or imply compositor acceptance.
 */
venus_ring_status_t venus_dmabuf_layout_validate(const venus_dmabuf_layout_t *layout);
/** @brief Validate an entire feedback table and tranche, then query a pair.
 * @param[in] table Nonnull immutable private bytes[table_bytes], borrowed for call.
 * @param[in] table_bytes Nonempty multiple of 16, at most 65536 bytes.
 * @param[in] indices Nonnull immutable private bytes[index_bytes], borrowed for call.
 * @param[in] index_bytes Nonempty even byte count, at most 8192 bytes.
 * @param[in] fourcc Requested DRM format. @param[in] modifier Exact requested modifier.
 * @return RingOk found, RingAgain absent, RingInvalid NULL, RingCorrupt invalid
 * lengths or out-of-range indices. Four unused table padding bytes are ignored.
 * @note Pure/allocation-free/thread-safe. Little-endian x86-64 native Wayland
 * schema. Complete tranche validation precedes success; no pointers retained.
 */
venus_ring_status_t venus_dmabuf_feedback_match(const void *table, size_t table_bytes,
                                                const void *indices, size_t index_bytes,
                                                uint32_t fourcc, uint64_t modifier);
/** @brief Private surface damage rectangle; signed Wayland coordinates, size 16.
 * @note Caller-owned immutable metadata, no storage ownership or pointers.
 */
typedef struct venus_dmabuf_damage_t {
    int32_t x;      /**< Nonnegative left pixel coordinate. */
    int32_t y;      /**< Nonnegative top pixel coordinate. */
    int32_t width;  /**< Positive width within image. */
    int32_t height; /**< Positive height within image. */
} venus_dmabuf_damage_t;
/** @brief Validate all damage rectangles before native protocol publication.
 * @param[in] width Image width 1..16384 pixels. @param[in] height Same for height.
 * @param[in] damage Nonnull immutable private array[count], borrowed for call.
 * @param[in] count Number of accessible rectangles, 1..64.
 * @return RingOk or RingInvalid for NULL/range/geometry errors, no mutation.
 * @note Pure/allocation-free/thread-safe; no pixel access or retained pointers.
 */
venus_ring_status_t venus_dmabuf_damage_validate(uint32_t width, uint32_t height,
                                                 const venus_dmabuf_damage_t *damage, size_t count);
_Static_assert(sizeof(venus_dmabuf_damage_t) == 16, "Damage C/Zig ABI");
_Static_assert(sizeof(venus_dmabuf_plane_t) == 24, "Plane C/Zig ABI");
_Static_assert(sizeof(venus_dmabuf_layout_t) == 120, "Layout C/Zig ABI");
#endif
