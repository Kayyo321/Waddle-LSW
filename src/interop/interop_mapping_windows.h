#ifndef WaddleInteropMappingWindowsH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleInteropMappingWindowsH
/**
 * @file interop_mapping_windows.h
 * @brief Retryable raw Windows IVSHMEM lease ownership for layout v3.
 *
 * This API does not initialize, read or validate shared-memory contents. One
 * serialized guest owner retains the opaque record until close and destroy
 * succeed. It must not copy the record, race callbacks or infer detach from EOF.
 * Native driver calls are separate from protocol and compositor acceptance.
 */
#include <stdint.h>

/** @brief Opaque, noncopyable owner allocated by create and freed by destroy. */
typedef struct interop_mapping_windows_t interop_mapping_windows_t;

/** @brief Local acquisition/cleanup result; not a protocol error code. */
typedef enum interop_mapping_windows_status_t {
    InteropMappingWindowsOk = 0, /**< Operation completed; consult its contract. */
    InteropMappingWindowsInvalidArgument = 1, /**< NULL output or invalid scalar. */
    InteropMappingWindowsOutOfMemory = 2, /**< Owner/detail allocation failed. */
    InteropMappingWindowsBusy = 3, /**< Owned resources forbid open/destroy. */
    InteropMappingWindowsOsError = 4, /**< Acquisition failed and fully unwound. */
    InteropMappingWindowsInvalidMapping = 5, /**< SDK detail/map data was malformed; fully unwound. */
    InteropMappingWindowsCleanupPending = 6, /**< Resources remain recorded; retry close. */
    InteropMappingWindowsMissingProof = 7 /**< Published map requires explicit quiescence assertion. */
} interop_mapping_windows_status_t;

/** @brief Local resource-presence diagnostics; never quiescence evidence. */
typedef enum interop_mapping_windows_resource_t {
    InteropMappingWindowsDeviceSet = 1, /**< A SetupAPI enumeration set remains owned. */
    InteropMappingWindowsDeviceHandle = 2, /**< An open driver handle remains owned. */
    InteropMappingWindowsDriverMap = 4 /**< A successful map IOCTL has not been released. */
} interop_mapping_windows_resource_t;

/**
 * @brief Borrowed raw mapping view, published only after complete open success.
 * @ownership No field transfers ownership. The view expires when a valid close
 * attempt begins, even if cleanup subsequently fails. No worker may resume use.
 */
typedef struct interop_mapping_windows_view_t {
    uintptr_t mapping_base; /**< Nonzero 64-byte-aligned driver address, not dereferenced here. */
    uint64_t mapping_length; /**< Exactly 0x80000000 bytes reported by the driver. */
    uint16_t peer_id; /**< Copied vendor peer identifier; not a protocol epoch. */
    uint16_t vectors; /**< Copied vendor interrupt count; no event is registered. */
    uint32_t reserved; /**< Always zero; no runtime interpretation. */
} interop_mapping_windows_view_t;

/** @brief Copied diagnostics; callers own this value and retain no native handle. */
typedef struct interop_mapping_windows_state_t {
    uint32_t owned_resources; /**< OR of local resource flags; zero permits destroy. */
    uint32_t primary_error; /**< First acquisition error, retained through cleanup. */
    uint32_t cleanup_error; /**< First cleanup error of the last attempt, or zero. */
    uint32_t reserved; /**< Always zero. */
} interop_mapping_windows_state_t;

/**
 * @brief Allocate an initially empty owner without opening a device.
 * @param[in,out] owner_out Non-NULL writable pointer initially containing NULL.
 * @return Ok, InvalidArgument, or OutOfMemory; every failure preserves output.
 * @note Caller owns the allocation until destroy succeeds. No Win32 resource is
 * acquired. Serialize publication of the returned pointer with all future calls.
 */
interop_mapping_windows_status_t interop_mapping_windows_create(
    interop_mapping_windows_t **owner_out);

/**
 * @brief Acquire a raw cached 2 GiB IVSHMEM lease through the pinned SDK ABI.
 * @param[in,out] owner Non-NULL live owner created here with no owned resources.
 * @param[in] device_index Zero-based SetupAPI interface index; no implicit selection.
 * @param[out] view_out Non-NULL writable, nonoverlapping borrowed-view result.
 * @return Ok publishes the complete view; every failure preserves view_out.
 * InvalidArgument/Busy cause no side effects. OutOfMemory/OsError/InvalidMapping
 * mean acquisition unwound fully. CleanupPending retains every resource whose
 * cleanup failed; inspect state and retry close before open or destroy.
 * @note No mapping bytes are touched and no layout validator is called. Owner
 * retains driver and enumeration handles immediately on acquisition. Detail
 * allocation is bounded to 65536 bytes and always freed before return. Caller
 * serializes open with every operation and all readers of a published view.
 */
interop_mapping_windows_status_t interop_mapping_windows_open(
    interop_mapping_windows_t *owner, uint32_t device_index,
    interop_mapping_windows_view_t *view_out);

/**
 * @brief Retry resource cleanup without forgetting any failed release/close.
 * @param[in,out] owner Non-NULL live owner created here, possibly partially open.
 * @param[in] detached_proof Exactly 0 or 1. A published, still-owned map requires
 * 1: caller asserts all local users stopped/joined and required host detachment.
 * An unpublished failed-open lease needs no external proof because no view was
 * handed out. This assertion does not independently establish quiescence.
 * @return InvalidArgument/MissingProof preserve state and perform no SDK calls.
 * Ok means all resources were relinquished; it is idempotent on an empty owner.
 * CleanupPending preserves resources whose cleanup failed. Map release failure
 * forbids closing its driver handle. Successful release clears map ownership
 * before handle close, so a close retry never releases that mapping twice.
 * Proof 1 remains required on retries while a published map remains owned;
 * after successful map release, handle/set-only retries may use proof 0.
 * Independent SetupAPI cleanup is still attempted. Primary error is retained;
 * cleanup_error records the first failed cleanup in this attempt, or zero.
 * @note Stop all view users before the first valid attempt. A published view is
 * expired at that point regardless of result. Do not reset/reuse the owner to
 * bypass a failure. Serialized caller only; no allocation occurs.
 */
interop_mapping_windows_status_t interop_mapping_windows_close(
    interop_mapping_windows_t *owner, uint32_t detached_proof);

/**
 * @brief Copy current diagnostics without consuming errors or resources.
 * @param[in] owner Non-NULL live owner created here; borrowed for this call.
 * @param[out] state_out Non-NULL writable result, not overlapping owner storage.
 * @return Ok after full copy or InvalidArgument with output unchanged.
 * @note No allocation or native call occurs. Serialize reads with owner writes.
 */
interop_mapping_windows_status_t interop_mapping_windows_get_state(
    const interop_mapping_windows_t *owner,
    interop_mapping_windows_state_t *state_out);

/**
 * @brief Free an empty owner and defensively NULL the caller's pointer.
 * @param[in,out] owner_inout Non-NULL writable owner pointer; NULL value is allowed.
 * @return Ok after free/NULL or for an already-NULL value; InvalidArgument for
 * NULL owner_inout; Busy if any native resource remains, preserving the pointer.
 * @note Caller must stop callbacks first and complete close. No forced native
 * cleanup occurs here and no failed ownership record can be silently discarded.
 */
interop_mapping_windows_status_t interop_mapping_windows_destroy(
    interop_mapping_windows_t **owner_inout);

#endif
