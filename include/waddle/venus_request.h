/** @file venus_request.h @brief Bounded portable receiver envelope codec. */
#ifndef WaddleVenusRequestH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusRequestH
#include "venus_ring.h"
/** @brief Fixed serialized header bytes, independent of decoded C padding. */
#define VenusRequestHeaderBytes 64u
/** @brief Maximum streamed payload/transfer bytes, before stricter owner limits. */
#define VenusRequestMaxPayload 16777216u
/** @brief Envelope operations; CPU completion semantics only. */
typedef enum venus_request_kind_t {
    RequestCapabilities = 1, /**< Query the pinned 160-byte public capset. */
    RequestSubmit = 2,       /**< Submit private Venus bytes; returns CPU fence. */
    RequestReply = 3,        /**< Copy completed CPU reply range. */
    RequestCreate = 4,       /**< Register bounded CPU SHM or existing device memory. */
    RequestFree = 5,         /**< Free registered resource after references end. */
    RequestRead = 6,         /**< Copy completed CPU SHM into response payload. */
    RequestWrite = 7,        /**< Copy request payload into CPU SHM. */
    RequestPoll = 8,         /**< Acquire latest CPU submission completion. */
    RequestGpuFence = 9,     /**< Fence an existing GPU queue, returns GPU identity. */
    RequestGpuPoll = 10      /**< Acquire an explicitly issued GPU fence retirement. */
} venus_request_kind_t;
/** @brief Unsigned stable wire status codes, never native signed enum bytes. */
typedef enum venus_request_status_t {
    RequestSuccess = 0,   /**< RingOk. */
    RequestAgain = 1,     /**< RingAgain; retry in a new sequence. */
    RequestInvalid = 2,   /**< RingInvalid. */
    RequestCorrupt = 3,   /**< RingCorrupt; terminal renderer/protocol failure. */
    RequestClosed = 4,    /**< RingClosed. */
    RequestCancelled = 5, /**< RingCancelled. */
    RequestTimeout = 6,   /**< RingTimeout. */
    RequestLimit = 7      /**< RingLimit; host quota exhaustion. */
} venus_request_status_t;
/** @brief Caller-owned decoded host values, not a serialized native struct.
 * @note No pointers/allocations; may be copied. Thread-safe on disjoint records.
 * Direction zero requests, one responses. Unspecified fields must be zero.
 * Full operation field/sequence rules are in IMPL_DESC.md's envelope contract.
 */
typedef struct venus_request_t {
    uint32_t kind;          /**< One of venus_request_kind_t. */
    uint32_t direction;     /**< Zero request, one response. */
    uint64_t sequence;      /**< Nonzero session exchange identity. */
    uint32_t payload_bytes; /**< Bytes following header, zero..16MiB. */
    uint32_t status;        /**< Wire status; requests always Success. */
    uint32_t resource_id;   /**< Request resource ID 2..65, otherwise zero. */
    uint32_t flags;         /**< Create flags only; resource policy validated. */
    uint64_t argument_zero; /**< Operation offset/blob/CPU or GPU fence, otherwise zero. */
    uint64_t argument_one;  /**< Requested bytes for resource/reply, otherwise zero. */
} venus_request_t;
/** @brief Validate and decode an immutable private envelope header.
 * @param[out] request Nonnull disjoint decoded record, zeroed on every error.
 * @param[in] frame Nonnull immutable private buffer[length], borrowed for call.
 * @param[in] length Exactly VenusRequestHeaderBytes; payload is not read here.
 * @return RingOk; RingInvalid for null arguments; RingCorrupt for wire errors.
 * @note Allocation-free/thread-safe on disjoint records. Validate sequence,
 * actual payload and live resource extent separately before dispatch.
 */
venus_ring_status_t venus_request_decode(venus_request_t *request, const void *frame,
                                         size_t length);
/** @brief Validate host values and encode a portable header.
 * @param[in] request Nonnull immutable private record, borrowed for call.
 * @param[out] frame Nonnull disjoint private buffer[length], unchanged on error.
 * @param[in] length Exactly VenusRequestHeaderBytes.
 * @return RingOk or RingInvalid for null/local fields/length.
 * @note Allocation-free/thread-safe on disjoint buffers. Writes no payload.
 */
venus_ring_status_t venus_request_encode(const venus_request_t *request, void *frame,
                                         size_t length);
_Static_assert(sizeof(venus_request_t) == 48, "Decoded C/Zig request ABI");
#endif
