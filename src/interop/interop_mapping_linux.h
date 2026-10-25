#ifndef WaddleInteropMappingLinuxH
#define WaddleInteropMappingLinuxH
/**
 * @file interop_mapping_linux.h
 * @brief Retryable Linux host ownership of one layout-v3 shared mapping.
 *
 * The owner duplicates a caller-owned descriptor, validates its explicitly
 * selected backing kind, and maps exactly 2 GiB. It never creates, truncates,
 * initializes, unlinks, or selects backing storage. Header validation takes
 * place only after the caller has selected a nonzero Hello epoch. Close is a
 * local unmap and descriptor release only; it does not prove peer drain,
 * compositor release, or safe backing-object reuse.
 */
#include "interop_layout.h"

#include <stdint.h>

/** @brief Opaque noncopyable mapping owner, allocated by create. */
typedef struct interop_mapping_linux_t interop_mapping_linux_t;

/** @brief Caller-selected backing object kind; never inferred from a pathname. */
typedef enum interop_mapping_linux_backing_t {
    InteropMappingLinuxBackingRegularPrivateFile = 1, /**< Owned regular file with exact private-file metadata. */
    InteropMappingLinuxBackingKvmfrCharacter = 2 /**< Existing KVMFR character device with validated capacity. */
} interop_mapping_linux_backing_t;

/** @brief Local mapping lifecycle result; not a guest protocol error code. */
typedef enum interop_mapping_linux_status_t {
    InteropMappingLinuxOk = 0, /**< Operation completed successfully. */
    InteropMappingLinuxInvalidArgument = 1, /**< Invalid pointer, enum, epoch, proof, or scalar. */
    InteropMappingLinuxWrongThread = 2, /**< Call did not run on the creating pthread. */
    InteropMappingLinuxOutOfMemory = 3, /**< Owner allocation failed; no OS resource acquired. */
    InteropMappingLinuxBusy = 4, /**< Current owner state does not permit this operation. */
    InteropMappingLinuxBackingRejected = 5, /**< Descriptor facts do not match selected backing kind. */
    InteropMappingLinuxOsError = 6, /**< OS acquisition or validation operation failed. */
    InteropMappingLinuxInvalidHeader = 7, /**< Snapshot failed frozen layout-v3 validation. */
    InteropMappingLinuxMissingQuiescence = 8, /**< Published view requires caller's local-user assertion. */
    InteropMappingLinuxCleanupPending = 9, /**< Unmap failed; mapping and descriptor remain owned for retry. */
    InteropMappingLinuxNotLockFree = 10, /**< A live shared atomic object is not lock-free on this host. */
    InteropMappingLinuxCloseDiagnostic = 11 /**< Linux close reported an error after ownership was relinquished. */
} interop_mapping_linux_status_t;

/** @brief Borrowed local mapping address published after header validation. */
typedef struct interop_mapping_linux_view_t {
    uintptr_t mapping_base; /**< Borrowed mapped address; expires at first valid close attempt. */
    uint64_t mapping_length; /**< Exactly 0x80000000 bytes. */
    uint32_t reserved[2]; /**< Always zero. */
} interop_mapping_linux_view_t;

/**
 * @brief Allocate an empty owner and bind it to the calling pthread.
 * @param[out] owner_out Non-NULL writable pointer; must initially contain NULL.
 * @return Ok and publishes the allocation, InvalidArgument, or OutOfMemory.
 * @note Caller owns the opaque allocation until destroy succeeds. No OS
 *       resource is acquired. All later calls must run on this pthread and be
 *       serialized; the owner must not be copied or concurrently accessed.
 */
interop_mapping_linux_status_t interop_mapping_linux_create(
    interop_mapping_linux_t **owner_out);

/**
 * @brief Duplicate, validate, and map an already-open backing object.
 * @param[in,out] owner Empty owner created on this pthread.
 * @param[in] borrowed_fd Caller-owned open descriptor; never closed or modified.
 * @param[in] backing_kind Explicit caller-selected object kind.
 * @return Ok after mapping is privately retained; InvalidArgument, WrongThread,
 *         Busy, BackingRejected, or OsError after full unwind. CloseDiagnostic
 *         means the duplicate was relinquished before Linux reported a close
 *         error during failed-open cleanup; errno is restored and no retry is
 *         permitted. Mapping cleanup never remains pending on this path.
 * @note RegularPrivateFile requires regular file, exact 2 GiB size, current
 *       effective uid, mode 0600, and one link. KvmfrCharacter requires a
 *       character device and exact 2 GiB capacity from the existing KVMFR
 *       capacity ioctl. Mapping is shared read/write. No bytes are written.
 *       Caller must keep object size and identity stable while owned; this
 *       descriptor cannot prevent external truncation.
 */
interop_mapping_linux_status_t interop_mapping_linux_open(
    interop_mapping_linux_t *owner, int borrowed_fd,
    interop_mapping_linux_backing_t backing_kind);

/**
 * @brief Validate the frozen header after Hello selected its nonzero epoch.
 * @param[in,out] owner Mapped owner with no previously published view.
 * @param[in] hello_epoch Nonzero epoch selected by the completed Hello exchange.
 * @param[out] snapshot Non-NULL aligned caller-owned result, unchanged on error.
 * @return Ok and publishes an immutable copied snapshot; InvalidArgument,
 *         WrongThread, Busy, InvalidHeader, NotLockFree, or OsError.
 * @note Copies ordinary bytes [0,56) and [72,4096) into private storage, then
 *       individually acquire-loads both live heartbeat atomics into offsets
 *       56 and 64; no live atomic is copied by memcpy. It checks native atomic
 *       lock freedom and delegates untrusted bytes to the Zig layout validator.
 *       Caller precondition: Hello has selected `hello_epoch`, and the peer's
 *       ordinary header fields are initialized and immutable for this session.
 *       Does not publish or retain the output snapshot.
 */
interop_mapping_linux_status_t interop_mapping_linux_validate_header(
    interop_mapping_linux_t *owner, uint64_t hello_epoch,
    interop_shm_header_snapshot_t *snapshot);

/**
 * @brief Publish the borrowed mapping view after successful header validation.
 * @param[in,out] owner Validated owner with no view already published.
 * @param[out] view_out Non-NULL writable caller-owned output, unchanged on error.
 * @return Ok, InvalidArgument, WrongThread, or Busy.
 * @ownership View fields transfer no ownership and are invalid at the beginning
 *            of the first valid close attempt, whether unmap succeeds or fails.
 * @note Local mapping access only. This operation proves no peer drain,
 *       compositor release, host detach, active-lease exclusion, or reuse safety.
 */
interop_mapping_linux_status_t interop_mapping_linux_view(
    interop_mapping_linux_t *owner,
    interop_mapping_linux_view_t *view_out);

/**
 * @brief Attempt local unmap and duplicated-descriptor release, retaining failures.
 * @param[in,out] owner Live owner created on this pthread.
 * @param[in] local_users_quiesced Exactly zero or one. A published view
 *        requires one, truthfully asserting all local readers have stopped and
 *        joined. It is not remote drain evidence and is never inferred.
 * @return Ok with no resources left, InvalidArgument, WrongThread,
 *         MissingQuiescence, CleanupPending, or CloseDiagnostic. The valid
 *         attempt expires a published view immediately and freezes mapping
 *         access until cleanup is complete. `CloseDiagnostic` means Linux
 *         reported a descriptor-close error after the descriptor number was
 *         relinquished; `errno` is restored to that close error and the owner
 *         has no descriptor to retry.
 * @note A failed munmap retains mapping and descriptor ownership, and blocks
 *       open, validation, and view calls until a later close succeeds.
 *       Successful unmap clears map ownership before one Linux close attempt;
 *       descriptor numbers are relinquished before close and never retried on
 *       any close error because Linux may already have released the number.
 */
interop_mapping_linux_status_t interop_mapping_linux_close(
    interop_mapping_linux_t *owner, uint32_t local_users_quiesced);

/**
 * @brief Free an empty owner and null the caller pointer.
 * @param[in,out] owner_inout Non-NULL writable pointer; its value may be NULL.
 * @return Ok for empty/already-NULL owner, InvalidArgument, WrongThread, or
 *         Busy while any OS resource remains; Busy preserves the pointer.
 * @note No implicit close occurs. The creating pthread must call destroy.
 */
interop_mapping_linux_status_t interop_mapping_linux_destroy(
    interop_mapping_linux_t **owner_inout);

#endif
