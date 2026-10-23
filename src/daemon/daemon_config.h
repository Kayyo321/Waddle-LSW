/**
 * @file daemon_config.h
 * @brief Subsystem daemon configuration structure and parser definitions.
 *
 * Defines daemon_config_t and C ABI interfaces implemented in Zig for parsing
 * and validating ~/.config/waddle/config.ini.
 */

#ifndef WaddleDaemonConfigH
#define WaddleDaemonConfigH

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

/** @brief Maximum USB vendor/product pairs stored for automatic attachment. */
#define ConfigMaxUsbAutoAttach UINT32_C(16)

/** @brief Maximum number of settings changed by one atomic config edit. */
#define ConfigEditMaxChanges UINT32_C(10)

/** @brief Network behavior selected for QEMU; config storage uses uint32_t. */
typedef enum config_network_mode_t {
    /** @brief QEMU user-mode networking with a virtio NIC. */
    NetworkModeUser = 0,
    /** @brief Explicitly disable guest networking. */
    NetworkModeNone = 1
} config_network_mode_t;

/** @brief QEMU audio backend selected for the guest. */
typedef enum config_audio_backend_t {
    /** @brief Use the host PipeWire audio backend. */
    AudioBackendPipewire = 0,
    /** @brief Use a silent guest render endpoint with no host playback/capture. */
    AudioBackendNone = 1
} config_audio_backend_t;

/** @brief IME text ownership mode. */
typedef enum config_input_ime_t {
    /** @brief Use guest-native IME processing. */
    InputImeGuest = 0,
    /** @brief Route text composition through the host. */
    InputImeHost = 1
} config_input_ime_t;

/** @brief Host keyboard shortcut inhibition policy. */
typedef enum config_input_inhibit_shortcuts_t {
    /** @brief Never request host shortcut inhibition. */
    InputInhibitShortcutsNever = 0,
    /** @brief Request inhibition while an exported window is fullscreen. */
    InputInhibitShortcutsFullscreen = 1,
    /** @brief Request inhibition while an exported window is focused. */
    InputInhibitShortcutsFocused = 2
} config_input_inhibit_shortcuts_t;

/** @brief Keyboard repeat ownership mode. */
typedef enum config_input_repeat_t {
    /** @brief Synthesize key repeat on the host. */
    InputRepeatHost = 0,
    /** @brief Disable host-synthesized key repeat. */
    InputRepeatOff = 1
} config_input_repeat_t;

/** @brief Application menu catalog filter. */
typedef enum config_integration_menu_apps_t {
    /** @brief Show installed applications and explicitly seeded applications. */
    IntegrationMenuAppsInstalled = 0,
    /** @brief Show every catalog application. */
    IntegrationMenuAppsAll = 1,
    /** @brief Hide catalog entries from the application menu. */
    IntegrationMenuAppsNone = 2
} config_integration_menu_apps_t;

/** @brief Guest desktop shortcut mirroring policy. */
typedef enum config_integration_desktop_shortcuts_t {
    /** @brief Mirror guest desktop shortcuts onto the host desktop. */
    IntegrationDesktopShortcutsMirror = 0,
    /** @brief Do not mirror guest desktop shortcuts. */
    IntegrationDesktopShortcutsOff = 1
} config_integration_desktop_shortcuts_t;

/** @brief Per-user executable MIME handler policy. */
typedef enum config_integration_exe_handler_t {
    /** @brief Register Waddle as the per-user executable handler. */
    IntegrationExeHandlerWaddle = 0,
    /** @brief Leave executable MIME handling unchanged. */
    IntegrationExeHandlerOff = 1
} config_integration_exe_handler_t;

/** @brief Windows UAC presentation mode. */
typedef enum config_integration_uac_t {
    /** @brief Export the genuine guest consent dialog as a host window. */
    IntegrationUacWindow = 0,
    /** @brief Suppress the prompt and report the resulting elevation. */
    IntegrationUacAuto = 1,
    /** @brief Send host Allow/Deny notifications and invoke guest UI Automation. */
    IntegrationUacNative = 2
} config_integration_uac_t;

/**
 * @brief One USB device selector for automatic QEMU attachment.
 * @note Values are numeric USB vendor/product identifiers. No pointers or
 *       ownership transfer; storage is embedded in daemon_config_t.
 */
typedef struct config_usb_id_t {
    /** @brief USB vendor ID, in the range 0..65535. */
    uint16_t vendor_id;
    /** @brief USB product ID, in the range 0..65535. */
    uint16_t product_id;
} config_usb_id_t;

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
    /** @brief Optional mdev parent PCI address; never passed through as a physical GPU. */
    char av_gpu_bdf[16];
    /** @brief Owned NUL-terminated lowercase UUID[36] of administrator-created mdev.
     * Empty means no GPU slice; lifetime is this config; no pointer transfer.
     * Callers synchronize writes, validation is read-only and allocation-free. */
    char av_gpu_mdev_uuid[37];

    /** @brief Network mode; one of config_network_mode_t. */
    uint32_t network_mode;
    /** @brief Enable CUPS TCP forwarding when the host socket exists (0/1). */
    uint32_t network_cups_forward;
    /** @brief Add the QEMU xHCI USB controller (0/1). */
    uint32_t usb_controller;
    /** @brief Number of active entries in usb_auto_attach. */
    uint32_t usb_auto_attach_count;
    /** @brief Fixed-capacity, duplicate-free USB IDs for automatic attachment. */
    config_usb_id_t usb_auto_attach[ConfigMaxUsbAutoAttach];
    /** @brief Audio backend; one of config_audio_backend_t. */
    uint32_t audio_backend;
    /** @brief Enable guest microphone input through the audio backend (0/1). */
    uint32_t audio_microphone;
    /** @brief Capture and expose per-application audio streams (0/1). */
    uint32_t audio_per_app;
    /** @brief IME mode; one of config_input_ime_t. */
    uint32_t input_ime;
    /** @brief Shortcut inhibition; one of config_input_inhibit_shortcuts_t. */
    uint32_t input_inhibit_shortcuts;
    /** @brief Keyboard repeat mode; one of config_input_repeat_t. */
    uint32_t input_repeat;
    /** @brief Enable clipboard synchronization (0/1). */
    uint32_t clipboard_enabled;
    /** @brief Clipboard/DnD transfer cap in MiB, range 1..1024. */
    uint32_t clipboard_max_mb;
    /** @brief Enable clipboard file copy/paste (0/1). */
    uint32_t clipboard_files;
    /** @brief Start host session and warm the VM at login (0/1). */
    uint32_t integration_start_at_login;
    /** @brief Menu catalog filter; one of config_integration_menu_apps_t. */
    uint32_t integration_menu_apps;
    /** @brief Desktop shortcut policy; one of config_integration_desktop_shortcuts_t. */
    uint32_t integration_desktop_shortcuts;
    /** @brief Executable MIME handler policy; one of config_integration_exe_handler_t. */
    uint32_t integration_exe_handler;
    /** @brief UAC presentation mode; one of config_integration_uac_t. */
    uint32_t integration_uac;
    /** @brief Enable desktop notifications (0/1). */
    uint32_t integration_notify;
} daemon_config_t;

/**
 * @brief Initializes a daemon configuration structure with default settings.
 *
 * Populates memory_mb (4096), vcpus (4), default VSOCK cid/port (3:5242),
 * default shell ("powershell.exe"), timeouts (60s/15s), resolves default disk image
 * path (~/.local/state/waddle/vm/windows.qcow2), and configures the default primary
 * mount exporting $HOME to Z:\ and initializes every seamless setting to the
 * defaults frozen in IMPL_DESC.md section 3.5, including an empty USB list.
 *
 * @param[out] config Nullable destination; NULL is a no-op. On success the
 * structure is caller-owned and contains no pointers or transferred storage.
 * @return No error result; initialization performs no allocation. Callers
 * synchronize concurrent access to the resulting structure.
 */
void daemon_config_init_defaults(daemon_config_t *config);

/**
 * @brief Parses an INI configuration string and updates the configuration structure.
 *
 * Missing options retain their previously set values. Any invalid values or
 * malformed lines cause the function to fail without partial corruption.
 *
 * @param[in,out] config   Nullable pointer to initialized caller-owned config;
 * NULL is rejected. Failure leaves every config byte unchanged.
 * @param[in]     ini_data Borrowed byte buffer; non-NULL when ini_len is nonzero.
 * @param[in]     ini_len  Length of ini_data in bytes, at most 65536.
 * @return 0 on success, or -1 on syntax, value, UTF-8, or validation error.
 * No input storage is retained or allocated. Thread-safe for separate configs.
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
 * @param[in,out] config Non-null pointer to caller-owned daemon_config_t; failed
 * parsing leaves its prior bytes unchanged.
 * @param[in]     path   Borrowed path to config.ini, or NULL for standard paths.
 * @return 0 on success (including file not present), or -1 on I/O/parse error.
 * The file buffer is local to the call. Callers synchronize shared config access.
 */
int daemon_config_load_file(daemon_config_t *config, const char *path);

/**
 * @brief Parses a single VirtIO-FS export string specification.
 *
 * Expected format: "<host_path>:<guest_drive>[:<rw|ro>]"
 * Example: "/home/dev:Z:\\:rw" or "/home/dev:Z:\\"
 *
 * @param[in]  line  Borrowed NUL-terminated specification; non-NULL.
 * @param[out] mount Non-null caller-owned destination, unchanged on failure.
 * @return 0 on success, or -1 on null, syntax, or validation error.
 * No allocation or pointer retention; thread-safe for separate destinations.
 */
int daemon_config_parse_export(const char *line, waddle_daemon_fs_mount_t *mount);

/**
 * @brief Validates all fields of a daemon_config_t structure against allowed bounds.
 *
 * @param[in] config Nullable pointer to caller-owned config; NULL is invalid.
 * @return 0 if valid, or -1 if any field is out of range or malformed.
 * Read-only, allocation-free, and thread-safe when the caller prevents writes.
 */
int daemon_config_validate(const daemon_config_t *config);

/**
 * @brief Validate and edit mutable INI values without changing unrelated bytes.
 * @param[in] data Non-null borrowed UTF-8 input, 1..65536 bytes.
 * @param[in] length Input byte count; embedded NUL is rejected.
 * @param[in] changes Non-null borrowed array of non-null NUL-terminated keys
 * (reset) or key=value assignments (set); unknown/duplicate keys are rejected.
 * New settings use exact qualified names such as `audio.backend`; existing
 * unqualified names keep their historical mapping, including `enabled` to
 * `av.enabled`.
 * @param[in] count Number of changes, 1 through ConfigEditMaxChanges.
 * @param[in] reset Nonzero selects documented default values.
 * @param[out] output Non-null caller-owned buffer; must not alias data.
 * @param[in] capacity Output capacity; complete output is limited to 65536 bytes.
 * @param[out] output_length Non-null byte count; zero on failure.
 * @return 0 on success, -1 on null, validation, or capacity failure. The output
 * length is zero on failure; output bytes are unspecified and must not persist.
 * No allocation or retained storage. Input/output must not overlap; calls are
 * thread-safe when callers use independent buffers.
 */
int daemon_config_edit(const char *data, size_t length,
                       const char *const *changes, size_t count, int reset,
                       char *output, size_t capacity, size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif /* WaddleDaemonConfigH */
