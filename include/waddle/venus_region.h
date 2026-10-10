#ifndef WaddleVenusRegionH
#define WaddleVenusRegionH
#include "venus_ring.h"

/** @brief Dedicated IVSHMEM region identity, immutable little-endian metadata. */
#define VenusRegionMagic 0x57564731u
/** @brief Region metadata extent in bytes; immutable, aligned to 64. */
#define VenusRegionHeaderBytes 64u
/** @brief Maximum dedicated BAR mapping bytes, limited to one GiB. */
#define VenusRegionMaxBytes 1073741824u

/** @brief Immutable host-initialized dedicated BAR layout, not the AV layout.
 * @note Exactly 64-byte size/alignment; caller-owned coherent mapping. All
 * offsets are bytes from mapping base; no pointers or dynamic allocations.
 */
typedef struct venus_region_header_t {
    _Alignas(64) uint32_t magic; /**< Always VenusRegionMagic. */
    uint32_t version;            /**< Always VenusRingVersion (one). */
    uint64_t mapping_bytes;      /**< Power of two, 4096..VenusRegionMaxBytes. */
    uint32_t capacity;           /**< Immutable capacity for each ring. */
    uint32_t command_offset;     /**< Always 64, guest producer/host consumer. */
    uint32_t reply_offset;       /**< 64 + ring header + capacity, opposite direction. */
    uint32_t resource_offset;    /**< First byte beyond the second ring. */
    uint8_t reserved[32];        /**< Zero immutable extension space. */
} venus_region_header_t;

/** @brief Borrowed local bidirectional view; attach/detach do not own memory.
 * @note Each ring retains its single producer/consumer rule. No concurrent
 * detach; mapping outlives all workers. Region metadata is not reread after attach.
 */
typedef struct venus_region_view_t {
    venus_ring_t commands; /**< Guest writes, host reads; borrowed endpoint. */
    venus_ring_t replies;  /**< Host writes, guest reads; borrowed endpoint. */
    uint8_t *resources;    /**< Borrowed remaining bytes; no allocator implied. */
    size_t resource_bytes; /**< Bounds of remaining region, immutable local snapshot. */
} venus_region_view_t;

/** @brief Host-initialize a dedicated quiescent BAR and two empty rings.
 * @param[in,out] mapping Nonnull, 64-byte-aligned borrowed writable region.
 * @param[in] mapping_bytes Power-of-two actual bytes, 4096..one GiB.
 * @param[in] capacity Power of two in ring range; both rings must fit.
 * @return RingOk or RingInvalid, invalid arguments do not change mapping.
 * @note Host initialization only before guest handoff; no allocation. Existing
 * AV mappings must never be passed here. Resource/payload bytes are untouched.
 */
venus_ring_status_t venus_region_init(void *mapping, size_t mapping_bytes, uint32_t capacity);
/** @brief Validate region ABI and attach its two rings without taking ownership.
 * @param[out] view Nonnull caller-owned output, disjoint from mapping.
 * @param[in,out] mapping Nonnull aligned initialized coherent mapping.
 * @param[in] mapping_bytes Actual accessible bytes, at least the declared extent.
 * @return RingOk or ring validation status; failure zeroes view.
 * @note Caller-thread only after handoff; metadata immutable during attachment.
 */
venus_ring_status_t venus_region_attach(venus_region_view_t *view, void *mapping,
                                        size_t mapping_bytes);
/** @brief Forget a quiescent local borrowed view.
 * @param[in,out] view Nullable local output; null is a no-op.
 * @note No free/close/unmap; caller-thread only after workers join.
 */
void venus_region_detach(venus_region_view_t *view);

_Static_assert(sizeof(venus_region_header_t) == 64, "Venus region size");
_Static_assert(_Alignof(venus_region_header_t) == 64, "Venus region alignment");
_Static_assert(offsetof(venus_region_header_t, mapping_bytes) == 8, "Venus region extent offset");
_Static_assert(offsetof(venus_region_header_t, reserved) == 32, "Venus region padding offset");
#endif
