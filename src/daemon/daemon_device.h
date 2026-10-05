/**
 * @file daemon_device.h
 * @brief Device profile management, registry discovery, and storage initialization.
 */

#ifndef WADDLE_DAEMON_DEVICE_H
#define WADDLE_DAEMON_DEVICE_H

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
 * @param[in] count Number of changes, 1..6; duplicates and unknown keys fail.
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

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_DEVICE_H */
