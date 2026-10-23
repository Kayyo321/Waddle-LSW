#ifndef WaddleVenusRingH
#define WaddleVenusRingH

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Version-one byte transport identity; immutable, no ownership. */
#define VenusRingMagic 0x57565231u
/** @brief ABI revision; immutable, little-endian x86-64 peers only. */
#define VenusRingVersion 1u
/** @brief Header extent in bytes, excluding caller-owned payload. */
#define VenusRingHeaderBytes 192u
/** @brief Smallest supported power-of-two payload capacity in bytes. */
#define VenusRingMinCapacity 64u
/** @brief Largest supported capacity in bytes, strictly below 2^31. */
#define VenusRingMaxCapacity 16777216u
/** @brief Atomic shutdown bit; either peer may set, never clear while active. */
#define VenusRingClosed 1u

/** @brief Transport outcomes; no allocation, ownership, or thread restrictions. */
typedef enum venus_ring_status_t {
    RingOk = 0,         /**< Exact transfer or successful lifecycle operation. */
    RingAgain = 1,      /**< Insufficient space/data; retry with cancellation checks. */
    RingInvalid = -1,   /**< Invalid local argument or incompatible mapping ABI. */
    RingCorrupt = -2,   /**< Invalid peer flags or occupied-byte distance. */
    RingClosed = -3,    /**< Shutdown observed; no payload transfer. */
    RingCancelled = -4, /**< Local wait cancelled; no new payload transfer. */
    RingTimeout = -5,   /**< Local wait deadline elapsed; no new payload transfer. */
    RingLimit = -6      /**< Receiver resource quota exceeded; no resource acquired. */
} venus_ring_status_t;

/** @brief Caller-owned, coherent IVSHMEM header; exactly three 64-byte lines.
 * @note Little-endian x86-64 only. Host initializes before control handoff.
 * Metadata and padding remain immutable; one producer owns tail, one consumer
 * owns head. Mapping outlives all endpoint operations. No pointers or locks.
 */
typedef struct venus_ring_header_t {
    _Alignas(64) uint32_t magic;        /**< VenusRingMagic, host initialized. */
    uint32_t version;                   /**< VenusRingVersion, host initialized. */
    uint32_t capacity;                  /**< Immutable power-of-two payload bytes. */
    uint32_t header_bytes;              /**< Always VenusRingHeaderBytes. */
    _Atomic uint32_t flags;             /**< Shutdown state; acquire/release access. */
    uint8_t reserved[44];               /**< Zero metadata extension space. */
    _Alignas(64) _Atomic uint32_t tail; /**< Release-published producer cursor. */
    uint8_t producer_padding[60];       /**< Zero, isolates producer writes. */
    _Alignas(64) _Atomic uint32_t head; /**< Release-published consumer cursor. */
    uint8_t consumer_padding[60];       /**< Zero, isolates consumer writes. */
} venus_ring_header_t;

/** @brief Local borrowed endpoint, not placed in IVSHMEM or sent on the wire.
 * @note Capacity is snapshotted during attach. Caller must not modify fields;
 * one thread per endpoint except close may race with operations. Detach requires
 * quiescence. No owned allocation; shared mapping outlives endpoint.
 */
typedef struct venus_ring_t {
    venus_ring_header_t *header; /**< Borrowed aligned header, null when detached. */
    uint8_t *payload;            /**< Borrowed capacity-byte region after header. */
    uint32_t capacity;           /**< Validated local capacity, never peer reread. */
} venus_ring_t;

/** @brief Initialize a quiescent shared subregion without touching payload.
 * @param[in,out] mapping Nonnull, 64-byte aligned borrowed mapping.
 * @param[in] mapping_bytes Accessible extent, at least header plus capacity.
 * @param[in] capacity Power of two within documented capacity limits.
 * @return RingOk or RingInvalid; invalid arguments do not modify mapping.
 * @note Initializer-thread only, both peers stopped; no allocation or unmap.
 */
venus_ring_status_t venus_ring_init(void *mapping, size_t mapping_bytes, uint32_t capacity);
/** @brief Validate and attach a local endpoint after initialization handoff.
 * @param[out] ring Nonnull caller-owned endpoint, disjoint from mapping.
 * @param[in,out] mapping Nonnull aligned borrowed coherent shared mapping.
 * @param[in] mapping_bytes Actual accessible bytes, not a peer-provided length.
 * @return RingOk, RingInvalid, RingCorrupt, or RingClosed. Failure zeroes ring.
 * @note Caller-thread only. Mapping must outlive endpoint; no allocation.
 */
venus_ring_status_t venus_ring_attach(venus_ring_t *ring, void *mapping, size_t mapping_bytes);
/** @brief Publish an exact bounded byte sequence or leave the ring untouched.
 * @param[in,out] ring Nonnull attached endpoint; producer-thread only.
 * @param[in] data Nonnull borrowed length-byte source, disjoint from mapping/ring.
 * @param[in] length Bytes, 1..capacity.
 * @return RingOk, RingAgain, RingInvalid, RingCorrupt, or RingClosed.
 * @note No allocation; release publication follows complete copy. Close may race.
 */
venus_ring_status_t venus_ring_write(venus_ring_t *ring, const void *data, size_t length);
/** @brief Consume an exact bounded byte sequence or leave output untouched.
 * @param[in,out] ring Nonnull attached endpoint; consumer-thread only.
 * @param[out] data Nonnull borrowed length-byte destination, disjoint from mapping/ring.
 * @param[in] length Bytes, 1..capacity.
 * @return RingOk, RingAgain, RingInvalid, RingCorrupt, or RingClosed.
 * @note Acquire precedes copy; release of head follows copy. No allocation.
 */
venus_ring_status_t venus_ring_read(venus_ring_t *ring, void *data, size_t length);
/** @brief Idempotently close an attached session without freeing resources.
 * @param[in,out] ring Nonnull attached endpoint, borrowed until workers stop.
 * @return RingOk or RingInvalid.
 * @note Thread-safe against read/write/close; join workers before detach/unmap.
 */
venus_ring_status_t venus_ring_close(venus_ring_t *ring);
/** @brief Clear a local endpoint after its operations stop.
 * @param[in,out] ring Nullable caller-owned endpoint; null is a no-op.
 * @note Caller-thread only, no free, close, or unmap; does not affect peer state.
 */
void venus_ring_detach(venus_ring_t *ring);

_Static_assert(sizeof(venus_ring_header_t) == VenusRingHeaderBytes, "Venus header size");
_Static_assert(_Alignof(venus_ring_header_t) == 64, "Venus header alignment");
_Static_assert(offsetof(venus_ring_header_t, flags) == 16, "Venus flags offset");
_Static_assert(offsetof(venus_ring_header_t, tail) == 64, "Venus producer offset");
_Static_assert(offsetof(venus_ring_header_t, head) == 128, "Venus consumer offset");
_Static_assert(sizeof(_Atomic uint32_t) == 4, "Venus atomic representation");
_Static_assert(_Alignof(_Atomic uint32_t) == 4, "Venus atomic alignment");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Cross-VM atomic locks forbidden");
_Static_assert(sizeof(void *) == 8, "Venus transport requires 64-bit peers");
#if !defined(__x86_64__) && !defined(_M_X64)
#error Venus transport requires little-endian x86-64 peers
#endif
#endif
