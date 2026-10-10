/**
 * @file daemon_device.h
 * @brief Device profile management, registry discovery, and storage initialization.
 */

#ifndef WaddleDaemonDeviceH
#define WaddleDaemonDeviceH

#include <stddef.h>
#include <stdint.h>
#include <waddle/daemon_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Name buffer capacity including NUL; ASCII identifiers only. */
#define WaddleMaxDeviceNameLen 64
/** @brief Registry capacity; excess entries return ENOSPC. */
#define MaxDeviceCount 32
/** @brief Initial RAM in MiB. */
#define DefaultDeviceMemoryMb 4096
/** @brief Initial processor count. */
#define DefaultDeviceVcpus 4
/** @brief First non-reserved guest CID. */
#define BaseVsockCid 3

/**
 * @brief Representation of an individual configured Waddle device subsystem.
 */
typedef struct device_info_t {
    char name[WaddleMaxDeviceNameLen];
    char config_path[WaddleMaxPathLen];
    char state_dir[WaddleMaxPathLen];
    char disk_image[WaddleMaxPathLen];
    char socket_path[WaddleMaxPathLen];
    uint32_t vsock_cid;
    uint32_t vsock_port;
    uint32_t memory_mb;
    uint32_t vcpus;
    /** @brief Reserved observation; discovery never contacts a live supervisor. */
    int is_running;
    /** @brief One only after bounded INI validation with explicit CID and disk. */
    int config_valid;
} device_info_t;

/**
 * @brief List of discovered Waddle device subsystems.
 */
typedef struct device_list_t {
    device_info_t devices[MaxDeviceCount];
    size_t count;
} device_list_t;

/**
 * @brief Validates that a device name contains only alphanumeric, hyphen, and underscore characters.
 *
 * @param[in] name Device identifier to validate.
 * @return 0 if valid, or -1 if NULL, empty, or containing invalid characters (sets errno to EINVAL).
 */
int daemon_device_validate_name(const char *name);

/**
 * @brief Purely resolves the configuration directory for Waddle devices.
 *
 * Typically resolves to ~/.config/waddle/devices or $XDG_CONFIG_HOME/waddle/devices.
 *
 * @param[out] out_path Destination buffer of at least WaddleMaxPathLen capacity.
 * @param[in]  path_cap Buffer capacity in bytes.
 * @return 0 on success, or -1 on error.
 */
int daemon_device_get_config_dir(char *out_path, size_t path_cap);

/**
 * @brief Purely resolves the persistent state directory for a specific device.
 *
 * Typically resolves to ~/.local/state/waddle/devices/<device-name>.
 *
 * @param[in]  device_name Name of the device.
 * @param[out] out_path    Destination buffer of at least WaddleMaxPathLen capacity.
 * @param[in]  path_cap    Buffer capacity in bytes.
 * @return 0 on success, or -1 on error.
 */
int daemon_device_get_state_dir(const char *device_name, char *out_path, size_t path_cap);

/**
 * @brief Purely resolves the UNIX domain socket path for a device daemon supervisor.
 *
 * If device_name is NULL or empty, resolves to the default socket path.
 *
 * @param[in]  device_name Optional name of the device (may be NULL).
 * @param[out] out_path    Destination buffer of at least WaddleMaxPathLen capacity.
 * @param[in]  path_cap    Buffer capacity in bytes.
 * @return 0 on success, or -1 on error.
 */
int daemon_device_get_socket_path(const char *device_name, char *out_path, size_t path_cap);

/**
 * @brief Scans the configuration directory and populates the device list.
 *
 * @param[out] list Pointer to device_list_t structure to populate.
 * @return Number of discovered devices (>= 0), or -1 on error.
 */
int daemon_device_list(device_list_t *list);

/**
 * @brief Acquire the stable registry inode's advisory whole-file lock.
 * @param[in] exclusive Nonzero obtains a write lock, creates private registry root/lock;
 * zero obtains a read lock without writes and returns ENOENT before first mutation.
 * @return Owned CLOEXEC fd, or -1 with errno (including EINTR). Caller closes to release.
 * @note Linux OFD locks: independent shared acquisitions may nest; never nest
 * an exclusive acquisition. Closing another descriptor cannot drop this lease.
 * Fork inherits the description until exec (CLOEXEC); children must not retain it.
 * No heap allocation; lock order is registry then per-device lease.
 */
int daemon_device_registry_lock(int exclusive);

/**
 * @brief Finds a specific device by name in the registry.
 *
 * @param[in]  name     Device identifier to look up.
 * @param[out] out_info Pointer to device_info_t to populate.
 * @return 0 if found, or -1 if not found (errno set to ENOENT).
 */
int daemon_device_find(const char *name, device_info_t *out_info);

/**
 * @brief Determines the next non-colliding VSOCK CID across existing devices.
 *
 * @param[in] list Active device list.
 * @return Next available CID (3..UINT32_MAX-1), or 0 with EINVAL/ENOSPC.
 * @note Borrowed list; no allocation or mutation; thread-safe. Caller holds registry write lock for reservation.
 */
uint32_t daemon_device_allocate_cid(const device_list_t *list);

/**
 * @brief Searches standard locations on the host system for a base Windows disk image.
 *
 * @param[out] out_path Destination buffer of at least WaddleMaxPathLen capacity.
 * @param[in]  path_cap Buffer capacity in bytes.
 * @return 0 if a valid base disk was found, or -1 otherwise (errno set to ENOENT).
 */
int daemon_device_find_base_disk(char *out_path, size_t path_cap);

/**
 * @brief Initializes a new device profile, creating its directories, disk, and configuration.
 *
 * If a base Windows disk is available, creates a copy-on-write QCOW2 overlay disk.
 *
 * @param[in]  name             Device identifier to create.
 * @param[in]  custom_base_disk Optional path to base disk image (may be NULL to auto-discover).
 * @param[out] out_info         Optional pointer to receive initialized device details (may be NULL).
 * @return 0 on success, or -1 on error (errno set appropriately).
 */
int daemon_device_init(const char *name, const char *custom_base_disk, device_info_t *out_info);

/**
 * @brief Read persistent selection without creating registry or starting a guest.
 * @param[out] name Non-null borrowed buffer; receives empty string if unset.
 * @param[in] capacity Buffer size including NUL, at least 64 for all names.
 * @return 0 on success, -1 with errno for invalid metadata, permissions or I/O.
 * @note No allocation; holds a shared registry lock during read. Do not nest locks.
 */
int daemon_device_default_get(char *name, size_t capacity);

/**
 * @brief Atomically replace default selection or clear it.
 * @param[in] name Borrowed valid registered healthy name, or NULL to clear idempotently.
 * @return 0 after directory fsync, -1 with errno. A post-rename fsync failure
 * leaves the new visible selection but its crash durability is uncertain.
 * @note No retained memory. Serializes under registry lock; does not boot or stop.
 * A unique 0600 staging file is flushed before atomic replacement. Caller must
 * not nest registry lock acquisition. Full multi-operation journaling is separate.
 */
int daemon_device_default_set(const char *name);

/**
 * @brief Apply validated mutable settings to a quiescent named device.
 * @param[in] name Non-null borrowed ASCII profile name.
 * @param[in] changes Non-null borrowed array of count non-null key/value strings.
 * @param[in] count Number of changes, 1..10; duplicates and unknown keys fail.
 * @param[in] reset Nonzero uses documented defaults, otherwise parses key=value.
 * @param[in] dry_run Nonzero validates without creating or changing files.
 * @return 0 on success; -1 with errno (EINVAL, ENOENT, EBUSY, EACCES or I/O).
 * @note Owns temporary fds through return; retains no pointers. Serializes registry
 * and holds the runtime lease through replacement. A parent fsync failure after
 * rename reports uncertain durability; the visible file is always complete.
 * Concurrent threads must not change process environment during the call.
 */
int daemon_device_config_update(const char *name, const char *const *changes,
                                size_t count, int reset, int dry_run);

/** @brief Storage operation identifiers; host-only, no wire representation. */
typedef enum device_operation_t { DeviceInit, DeviceRename, DeviceRemove, DeviceClone, DeviceExport, DeviceImport } device_operation_t;
/** @brief Borrowed mutation request; valid only for a synchronous call, no retained pointers.
 * Natural C alignment; caller initializes every field. Names non-null; argument is
 * optional base/size/new name/backup directory according to operation. Settings
 * apply to init; zero memory/vcpus select defaults. No concurrent environment edits.
 */
typedef struct device_request_t {
    device_operation_t operation; /**< Requested offline operation. */
    const char *name; /**< Source/target name, non-null ASCII. */
    const char *argument; /**< Optional borrowed operation argument. */
    const char *shell; /**< Optional UTF-8 init shell, at most 255 bytes. */
    uint32_t memory_mb; /**< Init RAM or zero default. */
    uint32_t vcpus; /**< Init CPUs or zero default. */
    int blank; /**< Init argument is explicit blank size. */
    int dry_run; /**< Nonzero forbids filesystem writes. */
    int keep_data; /**< Remove retains owned state and original configuration. */
} device_request_t;
/** @brief Execute validated offline transaction.
 * @param[in] request Non-null borrowed initialized operation.
 * @param[out] output Non-null 1024-byte buffer receiving retention/export path or empty.
 * @return 0 success, -1 errno EINVAL/ENOENT/EEXIST/EBUSY/EACCES/ENOSPC/EIO.
 * @note Owns temporary allocator/fds until return. Serializes registry before
 * runtime leases; rollback/commit journals survive interruption. Single-thread CLI.
 */
int daemon_device_mutate(const device_request_t *request, char output[WaddleMaxPathLen]);
/** @brief Recover journals under an already-held registry lock.
 * @param[in] repair Nonzero recovers; zero only checks and returns EUCLEAN if pending.
 * @return 0 clean/recovered; -1 errno. No retained memory; caller serializes writers.
 */
int daemon_device_recover(int repair);
/** @brief Scan under caller-held registry lock into non-null borrowed output.
 * @param[out] list Inline owned result.
 * @return Count or -1 errno; no allocation, no writes, no nested locks.
 */
int daemon_device_scan_locked(device_list_t *list);
/** @brief Open absolute no-follow directory components.
 * @param[in] path Non-null borrowed absolute path without dot components.
 * @param[in] create Nonzero creates absent 0700 components.
 * @return Owned CLOEXEC fd or -1 errno; caller closes. No retained pointers.
 */
int daemon_device_open_directory(const char *path, int create);
/** @brief Verify stopped ownership under caller-held registry lock.
 * @param[in] info Non-null healthy borrowed device descriptor.
 * @param[out] lease_fd Non-null receives owned lifetime fd or -1 if runtime absent.
 * @return 0 quiescent, -1 errno; caller closes even on failure. No runtime writes.
 */
int daemon_device_lock_quiescent(const device_info_t *info, int *lease_fd);

/** @brief Repair only proven stale, user-owned runtime socket names.
 * @param[in] info Non-null healthy borrowed profile; caller holds registry write lock.
 * @param[in] dry_run Nonzero validates the repair without unlinking anything.
 * @return 0 quiescent/repaired, -1 errno EBUSY/EACCES/ENOENT or I/O.
 * @note Acquires/releases a temporary lease and audits process/disk owners. Never
 * kills children, removes stable leases, or modifies registered config/disks.
 */
int daemon_device_repair_runtime(const device_info_t *info, int dry_run);
/** @brief Inspect a device without spawning it; optional explicit stale repair.
 * @param[in] name Non-null borrowed ASCII name.
 * @param[in] repair Nonzero recovers journals and proven stale sockets.
 * @param[in] dry_run Nonzero forbids writes even with repair.
 * @param[out] output Non-null bounded buffer for JSON diagnostics.
 * @param[in] capacity Output capacity including NUL; at least 65537 for full output.
 * @return 0 valid JSON result, -1 errno on selection/serialization/resource failure.
 * @note JSON reports findings independently of function status; caller owns buffer.
 * One call arena/fds are released on return; registry precedes runtime locks.
 */
int daemon_device_inspect(const char *name, int repair, int dry_run, char *output, size_t capacity);

/** @brief Journal an atomic registry metadata replacement under caller-held locks.
 * @param[in] path Non-null borrowed absolute device INI or default selection path.
 * @param[in] data Borrowed complete validated UTF-8 bytes, or NULL to remove.
 * @param[in] length Byte count 0..65536, zero required when data is NULL.
 * @return 0 committed, -1 errno; interruption leaves a recoverable journal.
 * @note Caller holds registry write lock and device lease for config edits. Arena,
 * staging descriptors and memory are released on return; no pointers retained.
 */
int daemon_device_replace_locked(const char *path, const char *data, size_t length);

/** @brief Observe a bound UNIX socket through the kernel table without connecting.
 * @param[in] path Non-null borrowed absolute socket pathname.
 * @return 1 live endpoint, 0 absent, -1 errno on lookup failure/ambiguity.
 * @note Bounded Zig parser; temporary allocation freed on every return. Thread-safe.
 */
int daemon_device_socket_in_use(const char *path);

/** @brief Install/restore SIGINT cancellation for one main-thread CLI command.
 * @param[in] enabled Nonzero installs, zero restores the previous disposition.
 * @return 0 success, -1 errno if sigaction fails; no allocation or borrowed data.
 * @note Main thread only; never used by guest execution or daemon service loops.
 */
int daemon_device_cancel_scope(int enabled);
/** @brief Read the signal-safe cancellation flag.
 * @return Nonzero after SIGINT; zero otherwise. No ownership; signal-safe.
 */
int daemon_device_cancelled(void);
/** @brief Publish the owned utility PID to the cancellation handler.
 * @param[in] pid Child PID, or zero to clear before releasing ownership.
 * @note Main thread only; an already pending SIGINT kills this child immediately.
 * No pointers or allocations; caller must reap the child on every path.
 */
void daemon_device_cancel_child(int pid);

#ifdef __cplusplus
}
#endif

#endif /* WaddleDaemonDeviceH */
