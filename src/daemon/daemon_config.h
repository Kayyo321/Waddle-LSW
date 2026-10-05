/**
 * @file daemon_config.h
 * @brief Subsystem daemon configuration structure and parser definitions.
 *
 * Defines daemon_config_t and C ABI interfaces implemented in Zig for parsing
 * and validating ~/.config/waddle/config.ini.
 */

#ifndef WADDLE_DAEMON_CONFIG_H
#define WADDLE_DAEMON_CONFIG_H

#include "waddle/daemon_protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Minimum allowable virtual machine RAM in megabytes (512 MiB). */
#define ConfigMinMemoryMb UINT32_C(512)

/** @brief Maximum allowable virtual machine RAM in megabytes (64 GiB). */
#define ConfigMaxMemoryMb UINT32_C(65536)

/** @brief Default virtual machine RAM in megabytes (4096 MiB). */
#define ConfigDefaultMemoryMb UINT32_C(4096)

/** @brief Minimum allowable virtual CPU core count. */
#define ConfigMinVcpus UINT32_C(1)

/** @brief Maximum allowable virtual CPU core count. */
#define ConfigMaxVcpus UINT32_C(128)

/** @brief Default virtual CPU core count. */
#define ConfigDefaultVcpus UINT32_C(4)

/** @brief Default start timeout in seconds. */
#define ConfigDefaultStartTimeoutSec UINT32_C(60)

/** @brief Default stop timeout in seconds. */
#define ConfigDefaultStopTimeoutSec UINT32_C(15)

/** @brief Maximum allowable start timeout in seconds (10 minutes). */
#define ConfigMaxStartTimeoutSec UINT32_C(600)

/** @brief Maximum allowable stop timeout in seconds (5 minutes). */
#define ConfigMaxStopTimeoutSec UINT32_C(300)

/**
 * @brief Parsed configuration settings for the Waddle background subsystem.
 */
typedef struct daemon_config_t {
    /** @brief Virtual machine memory in megabytes. */
    uint32_t memory_mb;
    /** @brief Number of virtual CPU cores. */
    uint32_t vcpus;
    /** @brief Absolute path to base Windows disk image (qcow2). */
    char disk_image[WaddleMaxPathLen];
    /** @brief VSOCK guest context ID (CID >= 3). */
    uint32_t vsock_cid;
    /** @brief VSOCK guest agent listener port (1 - UINT32_MAX). */
    uint32_t vsock_port;
    /** @brief Default shell binary spawned on zero-flag invocation (e.g. "powershell.exe"). */
    char default_shell[256];
    /** @brief Start readiness deadline in seconds. */
    uint32_t start_timeout_sec;
    /** @brief Graceful shutdown timeout in seconds before force kill. */
    uint32_t stop_timeout_sec;
    /** @brief Number of active VirtIO-FS shared directory mounts. */
    uint32_t mount_count;
    /** @brief Reserved for 8-byte alignment. */
    uint32_t reserved;
    /** @brief Array of VirtIO-FS directory export mappings. */
    waddle_daemon_fs_mount_t mounts[WaddleMaxMounts];
    /** @brief AV devices enabled (0/1); default disabled preserves CLI-only VMs. */
    uint32_t av_enabled;
    /** @brief Use managed persistent OVMF firmware (0/1); default retains BIOS boot. */
    uint32_t av_uefi;
    /** @brief Absolute private AV shared-memory/KVMFR path, no comma/newline. */
    char av_shm_path[WaddleMaxPathLen];
    /** @brief Explicit VFIO GPU PCI address dddd:bb:ss.f; empty means no passthrough. */
    char av_gpu_bdf[16];
} daemon_config_t;

/**
 * @brief Initializes a daemon configuration structure with default settings.
 *
 * Populates memory_mb (4096), vcpus (4), default VSOCK cid/port (3:5242),
 * default shell ("powershell.exe"), timeouts (60s/15s), resolves default disk image
 * path (~/.local/state/waddle/vm/windows.qcow2), and configures the default primary
 * mount exporting $HOME to Z:\.
 *
 * @param[out] config Non-null pointer to daemon_config_t to initialize.
 */
void daemon_config_init_defaults(daemon_config_t *config);

/**
 * @brief Parses an INI configuration string and updates the configuration structure.
 *
 * Missing options retain their previously set values. Any invalid values or
 * malformed lines cause the function to fail without partial corruption.
 *
 * @param[in,out] config   Non-null pointer to initialized daemon_config_t.
 * @param[in]     ini_data Pointer to INI configuration text.
 * @param[in]     ini_len  Length of ini_data in bytes.
 * @return 0 on success, or -1 on syntax or validation error.
 */
int daemon_config_parse_string(daemon_config_t *config,
                               const char *ini_data,
                               size_t ini_len);

/**
 * @brief Loads and parses a configuration file from the filesystem.
 *
 * If path is NULL, attempts to load from $XDG_CONFIG_HOME/waddle/config.ini or
 * ~/.config/waddle/config.ini. If the file does not exist, retains defaults and returns 0.
 * If the file exists but cannot be read or contains syntax errors, returns -1.
 *
 * @param[in,out] config Non-null pointer to daemon_config_t.
 * @param[in]     path   Path to config.ini, or NULL for standard default search paths.
 * @return 0 on success (including file not present), or -1 on parse/validation error.
 */
int daemon_config_load_file(daemon_config_t *config, const char *path);

/**
 * @brief Parses a single VirtIO-FS export string specification.
 *
 * Expected format: "<host_path>:<guest_drive>[:<rw|ro>]"
 * Example: "/home/dev:Z:\\:rw" or "/home/dev:Z:\\"
 *
 * @param[in]  line  NUL-terminated export specification string.
 * @param[out] mount Destination pointer for parsed mount descriptor.
 * @return 0 on success, or -1 on syntax/validation error.
 */
int daemon_config_parse_export(const char *line, waddle_daemon_fs_mount_t *mount);

/**
 * @brief Validates all fields of a daemon_config_t structure against allowed bounds.
 *
 * @param[in] config Non-null pointer to daemon_config_t to validate.
 * @return 0 if valid, or -1 if any field is out of range or malformed.
 */
int daemon_config_validate(const daemon_config_t *config);

/**
 * @brief Validate and edit mutable INI values without changing unrelated bytes.
 * @param[in] data Non-null borrowed UTF-8 input, at most 65536 bytes.
 * @param[in] length Input byte count; embedded NUL is rejected.
 * @param[in] changes Non-null borrowed array of non-null NUL-terminated keys
 * (reset) or key=value assignments (set); unknown/duplicate keys are rejected.
 * @param[in] count Number of changes, 1 through 10.
 * @param[in] reset Nonzero selects documented default values.
 * @param[out] output Non-null caller-owned buffer; must not alias data.
 * @param[in] capacity Output capacity; complete output is limited to 65536 bytes.
 * @param[out] output_length Non-null byte count; zero on failure.
 * @return 0 on success, -1 on validation/capacity failure. Output bytes are
 * unspecified on failure and must not be persisted. No allocation or retained
 * storage; thread-safe when callers use independent buffers.
 */
int daemon_config_edit(const char *data, size_t length,
                       const char *const *changes, size_t count, int reset,
                       char *output, size_t capacity, size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_CONFIG_H */
