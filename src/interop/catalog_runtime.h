#ifndef WaddleInteropCatalogRuntimeH
#define WaddleInteropCatalogRuntimeH
/**
 * @file catalog_runtime.h
 * @brief C ABI of the host catalog runtime (src/interop/catalog_runtime.zig).
 *
 * The runtime turns decoded guest Catalog* events and chunked icon transfers into published
 * desktop entries, menus and icons through the rooted manifest store. Contract:
 * impl/seamless-interop/claude_45_percent/catalog_runtime_contract.md.
 *
 * Threading: one runtime belongs to one control session and every call must come from the
 * session pump thread. Callers serialize all calls on a handle; distinct handles are independent.
 * Time: every timed call takes `now_ms`, an absolute CLOCK_MONOTONIC millisecond count supplied by
 * the caller. Wall-clock text is supplied as `updated`, a 20-byte UTC RFC 3339 timestamp such as
 * "2026-10-25T12:00:00Z", which is validated by the artifact producer.
 *
 * Pointers: every borrowed input pointer is valid only for the duration of the call. Optional
 * members are NULL when absent. Returned action key pointers are valid until the next call on the
 * same handle.
 */
#include <stddef.h>
#include <stdint.h>

/** @brief Opaque per-session runtime handle. */
typedef struct catalog_runtime_t catalog_runtime_t;

/** @brief Status values returned by catalog runtime calls. Zero is success. */
typedef enum catalog_runtime_status_t {
    CatalogRuntimeOk = 0,               /**< Applied, or nothing needed to change. */
    CatalogRuntimeInvalidArgument = 1,  /**< NULL or inconsistent argument, or unsupported configuration value. */
    CatalogRuntimeRejected = 2,         /**< Frame rejected; its state change was not applied. */
    CatalogRuntimeQuota = 3,            /**< A bounded capacity (apps, bytes, uncatalogued entries) is exhausted. */
    CatalogRuntimeOutOfMemory = 4,      /**< Allocation failed; prior state is unchanged where documented. */
    CatalogRuntimePending = 5,          /**< Publication is blocked or budgeted; retry from tick. */
    CatalogRuntimeStoreFailed = 6,      /**< Store or filesystem operation failed; prior published state is intact. */
    CatalogRuntimeUnsupported = 7       /**< Requested behaviour is not implemented (for example desktop_shortcuts=mirror). */
} catalog_runtime_status_t;

/** @brief Outbound action kinds from catalog_runtime_next_action. */
typedef enum catalog_runtime_action_kind_t {
    CatalogRuntimeActionIconRequest = 1,    /**< Send IconRequest(app_key, icon_size, transfer). */
    CatalogRuntimeActionTransferCancel = 2  /**< Send TransferCancel(transfer, status). */
} catalog_runtime_action_kind_t;

/** @brief Menu inclusion policy (`integration.menu_apps`). */
typedef enum catalog_runtime_menu_policy_t {
    CatalogRuntimeMenuInstalled = 0, /**< Non-System entries plus Seed entries. */
    CatalogRuntimeMenuAll = 1,       /**< Every entry. */
    CatalogRuntimeMenuNone = 2       /**< No menu entries; entries stay NoDisplay. */
} catalog_runtime_menu_policy_t;

/** @brief Desktop shortcut policy (`integration.desktop_shortcuts`). */
typedef enum catalog_runtime_shortcuts_t {
    CatalogRuntimeShortcutsNone = 0,  /**< No desktop shortcuts. */
    CatalogRuntimeShortcutsMirror = 1 /**< Rejected with CatalogRuntimeUnsupported in this milestone. */
} catalog_runtime_shortcuts_t;

/** @brief Interop status values carried by TransferCancel (IMPL_DESC 3.2.4). */
enum {
    CatalogRuntimeCancelNotFound = 1,    /**< Unknown or non-receiving transfer identifier. */
    CatalogRuntimeCancelCancelled = 3,   /**< Cancelled by the host (application removed or session reset). */
    CatalogRuntimeCancelTimedOut = 4,    /**< No progress within the 10 s transfer deadline. */
    CatalogRuntimeCancelTooLarge = 6,    /**< Announced size exceeds the 1 MiB icon cap or the retained budget. */
    CatalogRuntimeCancelUnsupported = 7, /**< MIME type other than image/png. */
    CatalogRuntimeCancelGuestError = 9   /**< Protocol or digest violation, or a non-PNG payload. */
};

/**
 * @brief Runtime creation inputs. Borrowed for the duration of catalog_runtime_create.
 *
 * Descriptors must be open directories: `manifest_dir_fd` is the private state directory (owner
 * only, mode 0700), `data_root_fd` and `config_root_fd` are the roots named by the matching
 * canonical absolute spellings. The runtime duplicates descriptors and never closes the caller's.
 */
typedef struct catalog_runtime_config_t {
    int manifest_dir_fd;               /**< Private state directory, mode 0700, current user. */
    int data_root_fd;                  /**< Data root descriptor (XDG_DATA_HOME). */
    int config_root_fd;                /**< Configuration root descriptor (XDG_CONFIG_HOME). */
    uint32_t menu_policy;              /**< catalog_runtime_menu_policy_t. */
    uint32_t shortcuts;                /**< catalog_runtime_shortcuts_t. */
    const uint8_t *data_root;          /**< Canonical absolute data root spelling. */
    size_t data_root_length;           /**< Length of data_root in bytes. */
    const uint8_t *config_root;        /**< Canonical absolute config root spelling. */
    size_t config_root_length;         /**< Length of config_root in bytes. */
    const uint8_t *launcher_path;      /**< Trusted canonical absolute launcher path. */
    size_t launcher_path_length;       /**< Length of launcher_path in bytes. */
    const uint8_t *device;             /**< Sanitized device identity, 1..64 bytes of [A-Za-z0-9_-]. */
    size_t device_length;              /**< Length of device in bytes. */
    const uint8_t *waddle_version;     /**< Creator version text, 1..64 bytes UTF-8. */
    size_t waddle_version_length;      /**< Length of waddle_version in bytes. */
} catalog_runtime_config_t;

/**
 * @brief One catalog application as decoded from CatalogAdd or CatalogUpdate.
 *
 * `key` must be a valid app key (3..200 bytes, `a-` AUMID form or `p-` 32-hex form). `display_name`
 * is 1..255 bytes of UTF-8 without NUL. `flags` must be within InteropAppFlagMask. `icon_sizes` is
 * a raw array of little-endian u16 sizes (even length, at most 14 bytes, strictly ascending, each
 * one of the frozen sizes). `content_hash`, when present, is exactly 32 bytes.
 */
typedef struct catalog_runtime_app_t {
    const uint8_t *key;          /**< App key bytes, borrowed. */
    size_t key_length;           /**< Key length in bytes. */
    const uint8_t *display_name; /**< Display name bytes, borrowed. */
    size_t display_name_length;  /**< Display name length in bytes. */
    uint32_t flags;              /**< InteropAppFlag bitmask. */
    const uint8_t *icon_sizes;   /**< Little-endian u16 size array, or NULL when absent. */
    size_t icon_sizes_length;    /**< Byte length of icon_sizes. */
    const uint8_t *content_hash; /**< 32-byte content hash, or NULL when absent. */
    size_t content_hash_length;  /**< Must be 32 when content_hash is non-NULL. */
} catalog_runtime_app_t;

/** @brief One outbound action produced by catalog_runtime_next_action. */
typedef struct catalog_runtime_action_t {
    uint32_t kind;          /**< catalog_runtime_action_kind_t. */
    uint32_t transfer;      /**< Transfer identifier to place in the frame. */
    uint32_t icon_size;     /**< Requested icon size for IconRequest; zero otherwise. */
    uint32_t status;        /**< CatalogRuntimeCancel* value for TransferCancel; zero otherwise. */
    const uint8_t *key;     /**< App key for IconRequest, valid until the next call; NULL otherwise. */
    size_t key_length;      /**< Length of key. */
} catalog_runtime_action_t;

/** @brief Diagnostic counters. */
typedef struct catalog_runtime_stats_t {
    uint32_t apps;                /**< Catalog applications in the snapshot. */
    uint32_t uncatalogued;        /**< Uncatalogued entries held by the runtime. */
    uint32_t outstanding;         /**< Open icon transfers. */
    uint64_t icon_bytes;          /**< Retained icon bytes. */
    uint32_t rejected_frames;     /**< Frames rejected since creation. */
    uint32_t publish_failures;    /**< Consecutive publication failures. */
    uint32_t sync_required;       /**< 1 when publication waits for a clean full sync. */
    uint32_t transaction_open;    /**< 1 while a CatalogBegin transaction is open. */
    uint32_t publication_pending; /**< 1 when publication work remains. */
    uint32_t dropped_actions;     /**< Actions dropped because the outbound ring was full. */
    uint32_t queued_actions;      /**< Actions currently queued for the owner. */
} catalog_runtime_stats_t;

/**
 * @brief Create a runtime, acquire the store lock and recover interrupted transactions.
 * @param[in] config Non-NULL configuration, borrowed for the call.
 * @param[out] out Non-NULL slot, set to the new handle on success and to NULL on every failure.
 * @return CatalogRuntimeOk; InvalidArgument for NULL or malformed input; Unsupported for mirror
 *         shortcuts; OutOfMemory; or StoreFailed when the lock is busy or recovery fails.
 * @note The handle is owned by the caller until catalog_runtime_destroy. Owner thread only.
 */
int catalog_runtime_create(const catalog_runtime_config_t *config, catalog_runtime_t **out);

/**
 * @brief Release a runtime, its snapshot, open transfers and the store lock.
 * @param[in] runtime Handle from create, or NULL (ignored). Must not be used afterwards.
 * @note Owner thread only; the session pump must have stopped calling the runtime.
 */
void catalog_runtime_destroy(catalog_runtime_t *runtime);

/**
 * @brief Open a full-sync transaction (CatalogBegin). A second Begin restarts the transaction.
 * @param[in] runtime Non-NULL handle.
 * @return CatalogRuntimeOk, or InvalidArgument for NULL.
 */
int catalog_runtime_begin(catalog_runtime_t *runtime);

/**
 * @brief Close the transaction (CatalogEnd). Applications not announced since Begin are removed.
 * @param[in] runtime Non-NULL handle.
 * @return CatalogRuntimeOk; Rejected when no transaction is open or the transaction was poisoned
 *         by an invalid frame, in which case the snapshot is kept and publication waits for a
 *         clean full sync.
 */
int catalog_runtime_end(catalog_runtime_t *runtime);

/**
 * @brief Apply CatalogAdd (is_update zero) or CatalogUpdate (is_update nonzero).
 * @param[in] runtime Non-NULL handle.
 * @param[in] is_update Zero for Add, nonzero for Update.
 * @param[in] app Non-NULL application view, borrowed for the call.
 * @return CatalogRuntimeOk; Rejected for invalid fields or an Update of an unknown key (the frame
 *         is ignored, and inside a transaction a duplicate or invalid frame also poisons it);
 *         Quota when the 4096-application bound is reached.
 */
int catalog_runtime_upsert(catalog_runtime_t *runtime, int is_update, const catalog_runtime_app_t *app);

/**
 * @brief Apply CatalogRemove. Unknown keys are a no-op.
 * @param[in] runtime Non-NULL handle.
 * @param[in] key App key bytes; NULL is allowed only with zero length (then a no-op).
 * @param[in] key_length Key length in bytes.
 * @return CatalogRuntimeOk, or InvalidArgument for NULL runtime.
 */
int catalog_runtime_remove(catalog_runtime_t *runtime, const uint8_t *key, size_t key_length);

/**
 * @brief Drop the in-memory snapshot and open transfers after a session change. Published files
 *        and the manifest are not modified.
 * @param[in] runtime Handle, or NULL (ignored).
 */
void catalog_runtime_reset_session(catalog_runtime_t *runtime);

/**
 * @brief Accept TransferBegin for an outstanding icon request.
 * @param[in] runtime Non-NULL handle.
 * @param[in] id Transfer identifier from the IconRequest.
 * @param[in] total Announced payload bytes (1..1 MiB and within the retained budget).
 * @param[in] mime MIME text, borrowed; must be "image/png".
 * @param[in] mime_length Length of mime.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @return CatalogRuntimeOk; Rejected for an unknown, non-awaiting, or refused identifier (the
 *         corresponding TransferCancel action is queued).
 */
int catalog_runtime_transfer_begin(catalog_runtime_t *runtime, uint32_t id, uint64_t total,
                                   const uint8_t *mime, size_t mime_length, uint64_t now_ms);

/**
 * @brief Accept one TransferChunk at a contiguous offset.
 * @param[in] runtime Non-NULL handle.
 * @param[in] id Transfer identifier.
 * @param[in] offset Byte offset, equal to the bytes already received.
 * @param[in] data Chunk bytes, borrowed; 1..61440 bytes.
 * @param[in] length Chunk length.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @return CatalogRuntimeOk; Rejected for a gap, overlap, oversize chunk, or unknown identifier.
 */
int catalog_runtime_transfer_chunk(catalog_runtime_t *runtime, uint32_t id, uint64_t offset,
                                   const uint8_t *data, size_t length, uint64_t now_ms);

/**
 * @brief Verify the SHA-256 digest and the PNG header, then store the icon.
 * @param[in] runtime Non-NULL handle.
 * @param[in] id Transfer identifier.
 * @param[in] hash Non-NULL 32-byte digest of the complete payload.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @return CatalogRuntimeOk on success; Rejected for a digest mismatch (TransferCancel GuestError
 *         queued), a non-PNG or wrong-size payload (dropped without cancel), or an unknown id.
 */
int catalog_runtime_transfer_end(catalog_runtime_t *runtime, uint32_t id, const uint8_t *hash, uint64_t now_ms);

/**
 * @brief Guest-side TransferCancel. Unknown identifiers are a no-op.
 * @param[in] runtime Non-NULL handle.
 * @param[in] id Transfer identifier.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @return CatalogRuntimeOk, or InvalidArgument for NULL runtime.
 */
int catalog_runtime_transfer_cancel(catalog_runtime_t *runtime, uint32_t id, uint64_t now_ms);

/**
 * @brief Expire stalled transfers, issue icon requests and publish pending artifacts.
 * @param[in] runtime Non-NULL handle.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] updated UTC RFC 3339 timestamp recorded in candidate manifests, borrowed.
 * @param[in] updated_length Length of updated (20 bytes).
 * @return CatalogRuntimeOk, or InvalidArgument for NULL runtime. Publication failures do not
 *         fail the tick; they are retried after a bounded backoff and counted in stats.
 */
int catalog_runtime_tick(catalog_runtime_t *runtime, uint64_t now_ms, const uint8_t *updated, size_t updated_length);

/**
 * @brief Ensure an uncatalogued window's NoDisplay desktop entry exists before its first commit.
 * @param[in] runtime Non-NULL handle.
 * @param[in] key Window app key, borrowed.
 * @param[in] key_length Key length.
 * @param[in] title Window title, borrowed; truncated to 64 bytes on a character boundary.
 * @param[in] title_length Title length.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @param[in] updated UTC RFC 3339 timestamp, borrowed.
 * @param[in] updated_length Length of updated.
 * @return CatalogRuntimeOk when published (or the key is a catalog entry); Pending while a
 *         transaction or sync requirement blocks it or the pass budget ran out; StoreFailed when
 *         the store refused it (retry is scheduled from now_ms); InvalidArgument for a bad key or
 *         title; Quota beyond 256 uncatalogued entries.
 */
int catalog_runtime_ensure_uncatalogued(catalog_runtime_t *runtime, const uint8_t *key, size_t key_length,
                                        const uint8_t *title, size_t title_length, uint64_t now_ms,
                                        const uint8_t *updated, size_t updated_length);

/**
 * @brief Pop the next outbound action into `out`.
 * @param[in] runtime Non-NULL handle.
 * @param[out] out Non-NULL action slot, written only when the return value is 1.
 * @return 1 when an action was written, 0 when none is queued or an argument is NULL.
 * @note Send each action before the next call; the key pointer is invalid afterwards.
 */
int catalog_runtime_next_action(catalog_runtime_t *runtime, catalog_runtime_action_t *out);

/**
 * @brief Earliest monotonic deadline at which the caller must tick.
 * @param[in] runtime Handle; NULL returns UINT64_MAX.
 * @param[in] now_ms Current monotonic time in milliseconds.
 * @return A deadline at or after now_ms, or UINT64_MAX when nothing is scheduled.
 */
uint64_t catalog_runtime_next_deadline(catalog_runtime_t *runtime, uint64_t now_ms);

/**
 * @brief Report whether an IconRequest for transfer `id` may still be sent.
 * @param[in] runtime Handle; NULL returns zero.
 * @param[in] id Transfer identifier.
 * @return Nonzero while the transfer still awaits its TransferBegin, zero once released.
 * @note Owner thread only. Use it before sending a request retained across callbacks.
 */
int catalog_runtime_request_live(catalog_runtime_t *runtime, uint32_t id);

/**
 * @brief Mark the feature sender as refusing frames. While blocked, no new IconRequest is issued.
 * @param[in] runtime Handle; NULL is ignored.
 * @param[in] blocked Nonzero while the sender reports QueueFull, zero once it accepts frames again.
 * @note Owner thread only.
 */
void catalog_runtime_set_send_blocked(catalog_runtime_t *runtime, int blocked);

/**
 * @brief Copy diagnostic counters.
 * @param[in] runtime Non-NULL handle.
 * @param[out] out Non-NULL output record, fully written on success.
 * @return CatalogRuntimeOk, or InvalidArgument for a NULL argument.
 */
int catalog_runtime_stats(catalog_runtime_t *runtime, catalog_runtime_stats_t *out);

#endif
