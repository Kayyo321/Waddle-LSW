#ifndef WaddleInteropMappingInitH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleInteropMappingInitH
/**
 * @file interop_mapping_init.h
 * @brief Initialize only frozen layout-v3 guest mapping metadata.
 *
 * The caller must own a fresh writable 2 GiB mapping and serialize this call
 * before any peer can read it. A nonzero proof value is only a caller-supplied
 * precondition; this API performs no operating-system mapping operation and
 * cannot prove that producers or host buffers are quiescent.
 */
#include <stdint.h>

/** @brief Exact guest metadata initialization result. */
typedef enum interop_mapping_init_status_t {
    InteropMappingInitStatusOk = 0, /**< Metadata is initialized for the supplied epoch. */
    InteropMappingInitStatusInvalidArgument = 1, /**< A pointer, size, alignment, epoch, or proof value is invalid. */
    InteropMappingInitStatusMissingProof = 2, /**< Caller did not assert exclusive ownership; mapping is unchanged. */
    InteropMappingInitStatusNotLockFree = 3 /**< Local C11 atomic probes are not lock-free; mapping is unchanged. */
} interop_mapping_init_status_t;

/**
 * @brief Initialize header and all fixed descriptors before a fresh guest Hello.
 * @param[in] mapping_base Borrowed actual mapping address; non-NULL, writable,
 *                         64-byte aligned, and valid for the full 2 GiB range.
 * @param[in] mapping_length Borrowed mapping size; must be exactly 2 GiB.
 * @param[in] session_epoch Fresh nonzero epoch chosen by the caller.
 * @param[in] exclusive_owner_proof Caller assertion, exactly 0 or 1; 1 does not
 *                                  independently establish external quiescence.
 * @return Ok after metadata initialization; InvalidArgument for invalid scalar
 *         facts, MissingProof when proof is 0, or NotLockFree when local atomic
 *         probes fail. Every non-Ok result leaves mapping bytes unchanged.
 * @note Allocation-free, retains no pointer, and owns no mapping. Writes only
 *       bytes [0, 0x4000); heap/audio regions remain untouched. `atomic_init`
 *       initializes atomic objects, but does not create a peer handshake.
 *       Caller serializes this call against every mapping reader/writer.
 */
interop_mapping_init_status_t interop_mapping_initialize(
    uintptr_t mapping_base,
    uint64_t mapping_length,
    uint64_t session_epoch,
    uint32_t exclusive_owner_proof);

#endif
