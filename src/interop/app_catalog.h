/** @file app_catalog.h @brief Bounded guest application catalog snapshots and diffs. */
#ifndef AppCatalogHeaderGuard
#define AppCatalogHeaderGuard

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_protocol.h"

#define AppCatalogMaxEntries 4096u /**< Maximum accepted candidates/entries; no allocation is implied. */
#define AppCatalogMaxBytes (16u * 1024u * 1024u) /**< Maximum aggregate borrowed input or owned snapshot bytes. */
#define AppCatalogMaxSourceBytes 4096u /**< Maximum source identifier byte length. */
#define AppCatalogMaxPathUnits 32767u /**< Maximum UTF-16 code units in a canonical path identity. */
#define AppCatalogCleanupMaxHandles 66u /**< Maximum nested search/file handles retained during one enumeration. */

/** @brief Result of an app catalog operation. */
typedef enum app_catalog_status_t {
    /** Operation succeeded and output was published. */
    AppCatalogStatusOk = 0,
    /** Required pointer, enum value, allocator, or empty-output precondition failed. */
    AppCatalogStatusInvalidArgument,
    /** UTF-8, package family, or AUMID bytes failed validation. */
    AppCatalogStatusInvalidText,
    /** Canonical UTF-16LE path identity failed validation or verification. */
    AppCatalogStatusInvalidPathIdentity,
    /** Candidate, snapshot, or enumeration bounds were exceeded. */
    AppCatalogStatusQuota,
    /** An owned allocation failed; output remains unchanged. */
    AppCatalogStatusOutOfMemory,
    /** A derived app key aliases distinct identity bytes; output remains unchanged. */
    AppCatalogStatusIdentityCollision,
    /** Enumeration cancellation was requested; output remains unchanged. */
    AppCatalogStatusCancelled,
    /** Enumeration deadline expired; output remains unchanged. */
    AppCatalogStatusTimedOut,
    /** Native enumeration is unavailable on this platform. */
    AppCatalogStatusUnsupportedPlatform,
    /** Native shell, filesystem, or canonicalization operation failed. */
    AppCatalogStatusNativeFailure
} app_catalog_status_t;

/** @brief Borrowed byte slice; NULL is valid only when length is zero.
 * @note The owner must keep data readable and immutable for the containing call.
 * No ownership is transferred; byte slices are not NUL terminated by contract.
 */
typedef struct app_catalog_bytes_t {
    const uint8_t *data; /**< Borrowed readable bytes. */
    size_t length;       /**< Byte length. */
} app_catalog_bytes_t;

/** @brief Allocator used for owned snapshots and diffs.
 * @note Both callbacks are required. allocate returns writable memory of the
 * exact requested size aligned for any C object (as malloc) or NULL. deallocate must release that exact allocation
 * with the same size. The context is borrowed, must outlive all snapshots and
 * diffs using it, and callbacks must be thread-safe if separate objects are
 * mutated concurrently. A single object still requires caller serialization.
 */
typedef struct app_catalog_allocator_t {
    void *context; /**< Borrowed callback context; may be NULL. */
    /** Allocate one owned object buffer.
     * @param[in] context Borrowed allocator context; may be NULL.
     * @param[in] size Positive byte count requested by the caller.
     * @return Writable memory of exactly size bytes, or NULL on failure.
     * @note The caller owns success until its matching deallocate callback.
     * Callback must be thread-safe for distinct concurrent allocations.
     */
    void *(*allocate)(void *context, size_t size);
    /** Release an allocation made by allocate.
     * @param[in] context Same borrowed context used for allocation.
     * @param[in] memory Non-NULL allocation returned by allocate.
     * @param[in] size Exact original allocation size.
     * @return No value; callback must release exactly once.
     * @note Must be thread-safe for distinct concurrent objects; caller
     * serializes operations on the same catalog object.
     */
    void (*deallocate)(void *context, void *memory, size_t size);
} app_catalog_allocator_t;

/** @brief Approved source category used for deterministic duplicate selection.
 * @note Values are not caller permissions to enumerate or write that source;
 * they describe already-observed read-only input records.
 */
typedef enum app_catalog_source_t {
    /** Windows AppsFolder shell items. */
    AppCatalogSourceAppsFolder = 1,
    /** Current-user Start Menu Programs tree. */
    AppCatalogSourceUserPrograms = 2,
    /** Common Start Menu Programs tree. */
    AppCatalogSourceCommonPrograms = 3,
    /** Current-user Desktop shortcut source. */
    AppCatalogSourceUserDesktop = 4,
    /** Public Desktop shortcut source. */
    AppCatalogSourcePublicDesktop = 5
} app_catalog_source_t;

/** @brief Identity form used to derive the frozen app key.
 * @note AUMID identity is exact UTF-8; canonical path identity is invariant
 * uppercase UTF-16LE without a terminator. The core checks byte syntax and
 * trusts its caller to supply path identity only from the approved native
 * final-path provider; raw wire strings are not canonical identity evidence.
 */
typedef enum app_catalog_identity_kind_t {
    /** Exact AUMID UTF-8 bytes. */
    AppCatalogIdentityAumid = 1,
    /** Canonical invariant-uppercase UTF-16LE target path bytes. */
    AppCatalogIdentityCanonicalPath = 2
} app_catalog_identity_kind_t;

/** @brief Borrowed input candidate. All slices are read only and not retained.
 * @note The caller owns every slice until app_catalog_snapshot_build returns.
 * Required and optional fields, source/flag pairing, and identity consistency
 * are validated before output publication. A path identity's filesystem and
 * Unicode-case provenance is a caller precondition; production Windows
 * candidates come from the native final-path provider, never raw wire fields.
 * No string is NUL terminated by contract. No candidate memory is retained.
 */
typedef struct app_catalog_candidate_t {
    app_catalog_source_t source; /**< Approved observed source category. */
    app_catalog_identity_kind_t identity_kind; /**< AUMID or canonical path identity. */
    app_catalog_bytes_t identity; /**< Exact AUMID UTF-8 or uppercase UTF-16LE canonical target. */
    app_catalog_bytes_t display_name; /**< Required UTF-8 display name. */
    app_catalog_bytes_t target_path; /**< Optional UTF-8 launch target; required for path identity. */
    app_catalog_bytes_t arguments; /**< Optional UTF-8 launch arguments. */
    app_catalog_bytes_t working_directory; /**< Required UTF-8 working directory. */
    app_catalog_bytes_t aumid; /**< Optional exact AUMID UTF-8; required to match AUMID identity. */
    app_catalog_bytes_t package_family; /**< Optional validated package family. */
    app_catalog_bytes_t source_identifier; /**< Stable UTF-8 source name/path for deterministic tie-breaking. */
    bool desktop_shortcut; /**< True only for a user/public Desktop .lnk observation. */
} app_catalog_candidate_t;

/** @brief Owned, immutable normalized entry. Slices belong to the containing snapshot.
 * @note Every byte slice is borrowed from the containing snapshot's storage and
 * remains readable until app_catalog_snapshot_deinit(); callers must not free
 * or mutate slices independently.
 */
typedef struct app_catalog_entry_t {
    app_catalog_bytes_t app_key; /**< Frozen protocol app key, not NUL terminated. */
    app_catalog_bytes_t identity; /**< Exact identity bytes used for collision detection. */
    app_catalog_identity_kind_t identity_kind; /**< Identity encoding. */
    app_catalog_bytes_t display_name; /**< UTF-8 display name. */
    app_catalog_bytes_t target_path; /**< UTF-8 target selected as a complete candidate tuple. */
    app_catalog_bytes_t arguments; /**< UTF-8 arguments. */
    app_catalog_bytes_t working_directory; /**< UTF-8 working directory. */
    app_catalog_bytes_t aumid; /**< Exact UTF-8 AUMID or empty. */
    uint32_t app_flags; /**< Only frozen observed Packaged/DesktopShortcut bits are set. */
} app_catalog_entry_t;

/** @brief Owned snapshot; all entry slices remain valid until deinit.
 * @note A zero-initialized or app_catalog_snapshot_init()'d object is empty.
 * A successful build transfers ownership of entries/storage and captures its
 * allocator. Caller must serialize build, read, and deinit on this object.
 */
typedef struct app_catalog_snapshot_t {
    app_catalog_allocator_t allocator; /**< Allocator retained for deinit. */
    app_catalog_entry_t *entries; /**< Owned sorted entry array. */
    size_t count; /**< Entry count. */
    size_t allocation_size; /**< Exact size of entries allocation. */
    uint8_t *storage; /**< Owned packed bytes for all entry slices. */
    size_t storage_size; /**< Exact size of storage allocation. */
    size_t duplicate_launch_conflicts; /**< Duplicate identities with differing launch tuples; selection remains deterministic. */
} app_catalog_snapshot_t;

/** @brief Catalog change kind; each value describes one sorted key transition. */
typedef enum app_catalog_change_kind_t {
    /** Key is absent before and present after. */
    AppCatalogChangeAdd = 1,
    /** Key exists on both sides but published fields differ. */
    AppCatalogChangeUpdate = 2,
    /** Key is present before and absent after. */
    AppCatalogChangeRemove = 3
} app_catalog_change_kind_t;

/** @brief One sorted diff item; entry pointers borrow from the input snapshots.
 * @note before/after and app_key remain valid only while their source snapshots
 * remain alive and unchanged.
 */
typedef struct app_catalog_change_t {
    app_catalog_change_kind_t kind; /**< Add, Update, or Remove. */
    app_catalog_bytes_t app_key; /**< Borrowed key from the selected entry. */
    const app_catalog_entry_t *before; /**< Borrowed prior entry; NULL for Add. */
    const app_catalog_entry_t *after; /**< Borrowed new entry; NULL for Remove. */
} app_catalog_change_t;

/** @brief Native handle kind retained after a failed Windows close operation. */
typedef enum app_catalog_cleanup_handle_kind_t {
    /** No pending handle is owned. */
    AppCatalogCleanupHandleNone = 0,
    /** The pending value is a Win32 HANDLE and must be closed with CloseHandle. */
    AppCatalogCleanupHandleWin32 = 1,
    /** The pending value is a FindFirstFileW search handle and must use FindClose. */
    AppCatalogCleanupHandleFind = 2
} app_catalog_cleanup_handle_kind_t;

/** @brief Caller-owned quarantine for native handles whose close failed.
 * @note Initialize before use. The adapter writes this only when native close
 * returns failure and keeps the exact handle value/type for retry. It is sized
 * for all 65 recursively open search handles plus one target handle. Caller
 * must keep this owner alive and unmodified until app_catalog_cleanup_owner_retry
 * succeeds; it must not copy, free, or close retained handles itself. Pending
 * handles block further enumeration with this owner. No allocation is made.
 */
typedef struct app_catalog_cleanup_owner_t {
    void *native_handles[AppCatalogCleanupMaxHandles]; /**< Opaque retained handles; caller must not access. */
    app_catalog_cleanup_handle_kind_t kinds[AppCatalogCleanupMaxHandles]; /**< Exact close operation per handle. */
    size_t count; /**< Number of retained handles, closed in reverse acquisition order. */
} app_catalog_cleanup_owner_t;

/** @brief Owned diff array borrowing entry data from both snapshots.
 * @note The changes array is released by app_catalog_diff_deinit(); each
 * change's entry pointers borrow from the two snapshots and are never freed by
 * diff deinit. Caller serializes use of this object.
 */
typedef struct app_catalog_diff_t {
    app_catalog_allocator_t allocator; /**< Allocator retained for deinit. */
    app_catalog_change_t *changes; /**< Owned sorted changes. */
    size_t count; /**< Number of changes. */
    size_t allocation_size; /**< Exact size of changes allocation. */
} app_catalog_diff_t;

/** @brief Optional synchronous cancellation callback.
 * @param[in] context Borrowed cancel_context below; may be NULL.
 * @return True to cancel enumeration, false to continue.
 * @note Callback is invoked synchronously on the enumeration thread, must not
 * retain references supplied by the enumerator, and should be nonblocking.
 */
typedef bool (*app_catalog_cancel_fn_t)(void *context);

/** @brief Native enumeration bounds and cancellation input.
 * @note options is borrowed for the call. deadline_monotonic_ns uses the same
 * absolute nanosecond monotonic clock as GetTickCount64 on Windows; zero disables
 * it. max_nodes must be 1..65536. Callback/context lifetime spans the call.
 */
typedef struct app_catalog_enumeration_options_t {
    uint64_t deadline_monotonic_ns; /**< Absolute monotonic deadline; zero disables deadline. */
    size_t max_nodes; /**< Maximum shell items/directories visited; must be 1..65536. */
    app_catalog_cancel_fn_t is_cancelled; /**< Optional callback checked between nodes. */
    void *cancel_context; /**< Borrowed callback context. */
    app_catalog_cleanup_owner_t *cleanup_owner; /**< Windows-only required empty caller-owned close-failure quarantine. */
} app_catalog_enumeration_options_t;

/** @brief Initialize a snapshot to the empty state.
 * @param[out] snapshot Non-NULL caller-owned output.
 * @return No error.
 * @note No allocation. Caller must serialize access to this object.
 */
void app_catalog_snapshot_init(app_catalog_snapshot_t *snapshot);

/** @brief Release an owned snapshot and reset it to empty.
 * @param[in,out] snapshot Non-NULL initialized snapshot.
 * @return No error; empty snapshots are accepted.
 * @note Frees with the allocator captured during build. Caller serializes use.
 */
void app_catalog_snapshot_deinit(app_catalog_snapshot_t *snapshot);

/** @brief Build a sorted immutable snapshot from borrowed candidates.
 * @param[in] allocator Non-NULL complete allocator callbacks.
 * @param[in] candidates Borrowed array; NULL only when count is zero.
 * @param[in] count Candidate count, at most AppCatalogMaxEntries.
 * @param[out] output Initialized empty snapshot; unchanged on failure.
 * @return Explicit validation, quota, collision, or allocation status.
 * @note Candidate bytes are not retained. Caller serializes output mutation. All typed pointers must be naturally
 * aligned; complete candidate arrays and their slices must be readable for the
 * call and disjoint from the output and allocator-owned buffers.
 */
app_catalog_status_t app_catalog_snapshot_build(app_catalog_allocator_t allocator,
                                                const app_catalog_candidate_t *candidates,
                                                size_t count,
                                                app_catalog_snapshot_t *output);

/** @brief Release an owned diff and reset it to empty.
 * @param[in,out] diff Non-NULL initialized diff.
 * @return No error; empty diffs are accepted.
 * @note Entry pointers in the diff are borrowed and do not get freed.
 */
void app_catalog_diff_deinit(app_catalog_diff_t *diff);

/** @brief Compute sorted Add/Update/Remove changes between snapshots.
 * @param[in] before Immutable prior snapshot.
 * @param[in] after Immutable new snapshot.
 * @param[out] output Initialized empty diff; unchanged on failure.
 * @return InvalidArgument, Quota, OutOfMemory, or Ok.
 * @note Diff uses the before snapshot's allocator and owns only its change array.
 * Both snapshots must be authentic unmodified outputs of snapshot_build,
 * remain immutable and outlive the diff; output must be disjoint from both.
 */
app_catalog_status_t app_catalog_diff_build(const app_catalog_snapshot_t *before,
                                            const app_catalog_snapshot_t *after,
                                            app_catalog_diff_t *output);

/** @brief Initialize an empty native-close quarantine owner.
 * @param[out] owner Non-NULL caller-owned record.
 * @return No error; does not allocate.
 * @note Caller serializes access and retains owner until no pending handle.
 */
void app_catalog_cleanup_owner_init(app_catalog_cleanup_owner_t *owner);

/** @brief Retry closing a handle previously retained after native close failure.
 * @param[in,out] owner Initialized owner; unchanged if retry still fails.
 * @return Ok if empty or successfully closed, NativeFailure if close fails,
 * or UnsupportedPlatform when a non-Windows build has a pending native handle.
 * @note Caller must retain and serialize owner access until Ok with count zero.
 * It is invalid to manually close or copy the retained handle.
 */
app_catalog_status_t app_catalog_cleanup_owner_retry(app_catalog_cleanup_owner_t *owner);

/** @brief Enumerate supported Windows shell sources and build a snapshot.
 * @param[in] allocator Non-NULL complete allocator callbacks.
 * @param[in] options Non-NULL bounded options with node cap 1..65536 and an
 * empty cleanup_owner on Windows.
 * @param[out] output Initialized empty snapshot; unchanged on failure.
 * @return NativeFailure, Cancelled, TimedOut, UnsupportedPlatform, or core status.
 * @note Windows only. Does not launch applications or call
 * IShellLink::Resolve, and does not mutate application data. For path identity
 * it opens each existing target with read-only FILE_READ_ATTRIBUTES access to
 * obtain final path metadata, then closes the handle. Caller invokes on a
 * thread that permits STA initialization; the adapter initializes and
 * balances its COM apartment during the call. If a filesystem or final-path
 * handle close fails, its exact native handle is quarantined in cleanup_owner;
 * caller must retry-close it before reuse or release. Caller serializes owner
 * and output access. On non-Windows returns
 * UnsupportedPlatform without mutation.
 */
app_catalog_status_t app_catalog_enumerate(app_catalog_allocator_t allocator,
                                           const app_catalog_enumeration_options_t *options,
                                           app_catalog_snapshot_t *output);

#endif
