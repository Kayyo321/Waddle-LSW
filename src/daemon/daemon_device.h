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

#define WaddleMaxDeviceNameLen 64
#define MaxDeviceCount 32
#define DefaultDeviceMemoryMb 4096
#define DefaultDeviceVcpus 4
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
    int is_running;
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
 * @brief Resolves the configuration directory for Waddle devices.
 *
 * Typically resolves to ~/.config/waddle/devices or $XDG_CONFIG_HOME/waddle/devices.
 *
 * @param[out] out_path Destination buffer of at least WaddleMaxPathLen capacity.
 * @param[in]  path_cap Buffer capacity in bytes.
 * @return 0 on success, or -1 on error.
 */
int daemon_device_get_config_dir(char *out_path, size_t path_cap);

/**
 * @brief Resolves the persistent state directory for a specific device.
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
 * @brief Resolves the UNIX domain socket path for a device daemon supervisor.
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
 * @return Next available CID (>= BaseVsockCid).
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

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_DEVICE_H */
