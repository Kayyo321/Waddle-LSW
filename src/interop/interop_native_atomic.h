#ifndef WaddleInteropNativeAtomicH
#define WaddleInteropNativeAtomicH
/**
 * @file interop_native_atomic.h
 * @brief C11 helpers for layout-v3 slot ownership and heartbeats.
 *
 * Preserves the frozen mapping structs and wire ABI in waddle/interop_memory.h.
 * Calls borrow
 * an existing, accessible, 64-byte-aligned 2 GiB mapping; they do not map,
 * initialize, copy metadata, allocate, pin, free, or retain any storage.
 */
#include "waddle/interop_memory.h"
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Exact mapping size required by these native helpers. */
#define InteropNativeMappingBytes UINT64_C(0x80000000)

/** @brief Offset of the first descriptor in the frozen mapping. */
#define InteropNativeDescriptorOffset UINT64_C(0x1000)
/** @brief Exact descriptor count in layout v3. */
#define InteropNativeDescriptorCount UINT32_C(192)

#if !defined(__x86_64__) && !defined(_M_X64)
#error Native interop atomics require x86_64
#endif

/** @brief Frozen state values already encoded in interop_slot_descriptor_t. */
typedef enum interop_slot_state_t {
    InteropSlotFree = 0, /**< No metadata access; next producer may claim ownership. */
    InteropSlotWriting = 1, /**< Producer owns mutable metadata. */
    InteropSlotReady = 2, /**< Published immutable metadata awaits a consumer. */
    InteropSlotConsuming = 3 /**< Consumer owns immutable metadata and pixel use. */
} interop_slot_state_t;

/** @brief Select one of the two frozen shared-header heartbeat fields. */
typedef enum interop_heartbeat_kind_t {
    InteropGuestHeartbeat = 0, /**< Guest-owned single-writer counter. */
    InteropHostHeartbeat = 1 /**< Host-owned single-writer counter. */
} interop_heartbeat_kind_t;

/** @brief Result of one local native atomic helper call. */
typedef enum interop_atomic_status_t {
    InteropAtomicStatusOk = 0, /**< Requested operation completed. */
    InteropAtomicStatusInvalidArgument = 1, /**< Local pointer/range/selector/pair is invalid. */
    InteropAtomicStatusWrongState = 2, /**< State mismatch or non-increasing heartbeat. */
    InteropAtomicStatusNotLockFree = 3, /**< Atomic object is unsupported on this target. */
    InteropAtomicStatusCounterExhausted = 4 /**< Heartbeat reached UINT64_MAX; reconnect required. */
} interop_atomic_status_t;

_Static_assert(sizeof(void *) == 8, "native interop atomic adapter supports x86_64 only");
_Static_assert(sizeof(uint32_t) == 4, "slot state width is frozen at 32 bits");
_Static_assert(sizeof(uint64_t) == 8, "heartbeat width is frozen at 64 bits");
_Static_assert(sizeof(_Atomic uint32_t) == 4, "atomic slot state must retain 32-bit width");
_Static_assert(sizeof(_Atomic uint64_t) == 8, "atomic heartbeat must retain 64-bit width");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "x86_64 int atomics must be always lock-free");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2, "x86_64 long atomics must be always lock-free");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "x86_64 long long atomics must be always lock-free");
_Static_assert(sizeof(interop_slot_state_t) == 4, "local state values use 32-bit C enum ABI");
_Static_assert(sizeof(interop_heartbeat_kind_t) == 4, "local heartbeat selector uses 32-bit C enum ABI");
_Static_assert(sizeof(interop_atomic_status_t) == 4, "local status uses 32-bit C enum ABI");

/**
 * Validate the addresses and lock-free properties of the header heartbeat
 * objects and one descriptor state object in an existing layout-v3 mapping.
 *
 * @param[in] mapping_base Actual mapped base address, non-NULL and 64-byte aligned.
 * @param[in] mapping_length Exact actual mapping length; must be 2 GiB.
 * @param[in] header Header at mapping_base; caller guarantees readable mapped
 *                   storage. Non-atomic header values must already be validated.
 * @param[in] descriptor One exact 64-byte descriptor at offset 0x1000 + index*64,
 *                       index in [0,192); caller guarantees accessible initialized storage.
 * @return Ok when guest_heartbeat, host_heartbeat, and descriptor.state are
 *         all lock-free; InvalidArgument for null, wrong-sized, misaligned,
 *         wrapped, out-of-range, or inconsistent pointers; NotLockFree when an
 *         actual atomic object is not lock-free on this target.
 * @note Performs no atomic load/store/CAS and changes no bytes. It cannot prove
 *       mapping accessibility, initialization, protocol validation, exclusive
 *       ownership, or that a caller-provided descriptor is semantically valid.
 *       Safe to call concurrently for an immutable mapping.
 */
interop_atomic_status_t interop_atomic_validate_mapping(
    uintptr_t mapping_base,
    uint64_t mapping_length,
    const interop_shm_header_t *header,
    const interop_slot_descriptor_t *descriptor);

/**
 * Perform exactly one legal state transition by strong compare-exchange.
 *
 * Allowed pairs are Free->Writing, Writing->Ready, Ready->Consuming,
 * Consuming->Free, and Writing->Free (cancel before publication). Every other
 * from/to pair is rejected as InvalidArgument before any atomic access. A
 * permitted pair whose observed state differs from from_state returns
 * WrongState and leaves the state unchanged.
 *
 * @param[in,out] descriptor Descriptor in an accessible, initialized mapping;
 *                           this caller must own the current from_state phase.
 * @param[in] mapping_base Actual aligned 2 GiB mapping base.
 * @param[in] mapping_length Exact actual mapping length.
 * @param[in] from_state Expected frozen state value.
 * @param[in] to_state Requested frozen state value.
 * @return Ok after exactly one transition, InvalidArgument for invalid pair or
 *         pointer/range/alignment or address outside the fixed descriptor table, WrongState for failed compare-exchange, or
 *         NotLockFree if descriptor.state is not lock-free.
 * @note CAS success uses memory_order_acq_rel and failure uses memory_order_relaxed.
 *       The acquire side of Free->Writing observes the previous release of Free;
 *       Writing->Ready publishes descriptor metadata; Ready->Consuming acquires
 *       that metadata; Consuming->Free releases ownership for the next writer.
 *       Writing->Free releases a canceled partial write that no consumer saw.
 *       Caller must access ordinary metadata only after owning Writing or
 *       Consuming and stop accessing it before publishing Ready or Free.
 *       Does not validate metadata, copy it, initialize the initial Free state,
 *       or detect a forged state/ownership claim.
 */
interop_atomic_status_t interop_slot_transition(
    interop_slot_descriptor_t *descriptor,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    uint32_t from_state,
    uint32_t to_state);

/**
 * Atomically read one selected heartbeat value.
 *
 * @param[in] header Validated header located at mapping_base in an accessible mapping.
 * @param[in] mapping_base Actual aligned 2 GiB mapping base.
 * @param[in] mapping_length Exact actual mapping length.
 * @param[in] kind GuestHeartbeat or HostHeartbeat.
 * @param[out] value Non-NULL aligned caller-owned output, disjoint from the
 *                   complete mapping range and unchanged on error. Its address
 *                   plus sizeof(*value) must not overflow uintptr_t.
 * @return Ok after a relaxed atomic load, InvalidArgument for invalid pointer,
 *         range, alignment, output alias or selector, or NotLockFree when the
 *         selected object is not lock-free.
 * @note No allocation or retained pointer. Heartbeat reads do not synchronize
 *       descriptor metadata or establish slot ownership.
 */
interop_atomic_status_t interop_heartbeat_load(
    const interop_shm_header_t *header,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    interop_heartbeat_kind_t kind,
    uint64_t *value);

/**
 * Atomically store a caller-computed strictly increasing heartbeat value.
 *
 * @param[in,out] header Validated header located at mapping_base in an accessible mapping.
 * @param[in] mapping_base Actual aligned 2 GiB mapping base.
 * @param[in] mapping_length Exact actual mapping length.
 * @param[in] kind GuestHeartbeat or HostHeartbeat; this caller must own its writer.
 * @param[in] next_value Nonzero value greater than the currently stored value.
 * @return Ok after a relaxed atomic store, InvalidArgument for invalid pointer,
 *         range, alignment, selector or zero value, WrongState if next_value is
 *         not strictly greater, CounterExhausted if the current value is UINT64_MAX,
 *         or NotLockFree when the selected object is not lock-free.
 * @note Uses a relaxed atomic load followed by a relaxed atomic store under the
 *       single-writer-per-field precondition. The caller computes the next value
 *       without wrapping; the helper rejects non-increasing input and never wraps.
 *       Concurrent readers are supported; concurrent writers to one field are not.
 */
interop_atomic_status_t interop_heartbeat_store_next(
    interop_shm_header_t *header,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    interop_heartbeat_kind_t kind,
    uint64_t next_value);

#endif
