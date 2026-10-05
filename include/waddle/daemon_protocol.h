/**
 * @file daemon_protocol.h
 * @brief Wire protocol, data structures, and framing for the Waddle daemon supervisor.
 *
 * This header defines the C ABI wire structures, message types, subsystem operational
 * states, and status descriptors exchanged between the Waddle CLI client (`waddle`)
 * and the background subsystem supervisor daemon (`waddled`) over local UNIX domain sockets.
 */

#ifndef WADDLE_DAEMON_PROTOCOL_H
#define WADDLE_DAEMON_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Magic identifier value: 'WADD' in Little-Endian byte order. */
#define WaddleDaemonMagic UINT32_C(0x57414444)

/** @brief Current daemon wire protocol version number. */
#define WaddleDaemonVersion UINT16_C(1)

/** @brief Maximum allowed payload size for daemon control messages (64 KiB). */
#define WaddleDaemonMaxPayloadSize UINT32_C(65536)

/** @brief Fixed wire header size in bytes. */
#define WaddleDaemonHeaderSize UINT32_C(16)

/** @brief Maximum length for filesystem path strings including NUL terminator. */
#define WaddleMaxPathLen UINT32_C(1024)

/** @brief Maximum number of VirtIO-FS shared directory mounts supported in status. */
#define WaddleMaxMounts UINT32_C(8)

/** @brief Default start timeout in seconds. */
#define WaddleDefaultStartTimeoutSec UINT32_C(60)

/** @brief Default graceful shutdown timeout in seconds before force kill. */
#define WaddleDefaultStopTimeoutSec UINT32_C(15)

/** @brief Internal stop mode: ACPI only; timeout preserves VM/resources and returns ETIMEDOUT.
 * Immutable, allocation-free. Serialized as a separate request ID with force=0
 * so older daemons reject it rather than interpreting it as forced termination.
 */
#define WaddleStopGracefulOnly UINT32_C(2)

/** @brief Default guest Context Identifier for VSOCK. */
#define WaddleDefaultVsockCid UINT32_C(3)

/** @brief Default guest VSOCK port. */
#define WaddleDefaultVsockPortVal UINT32_C(5242)

/* Backward compatibility aliases */
#define WADDLE_DAEMON_MAGIC WaddleDaemonMagic
#define WADDLE_DAEMON_VERSION WaddleDaemonVersion
#define WADDLE_DAEMON_MAX_PAYLOAD_SIZE WaddleDaemonMaxPayloadSize
#define WADDLE_MAX_PATH_LEN WaddleMaxPathLen
#define WADDLE_MAX_MOUNTS WaddleMaxMounts

/**
 * @brief Enumeration of daemon IPC message types.
 */
typedef enum waddle_daemon_msg_type_t {
    /** @brief Unspecified / invalid message type. */
    DaemonMsgNone             = 0x0000,
    /** @brief Host CLI to Daemon: Request subsystem startup. */
    DaemonMsgStartReq         = 0x0001,
    /** @brief Daemon to Host CLI: Result of start request. */
    DaemonMsgStartResp        = 0x0002,
    /** @brief Host CLI to Daemon: Request graceful subsystem shutdown. */
    DaemonMsgStopReq          = 0x0003,
    /** @brief Daemon to Host CLI: Result of stop request. */
    DaemonMsgStopResp         = 0x0004,
    /** @brief Host CLI to Daemon: Query subsystem health and metrics. */
    DaemonMsgStatusReq        = 0x0005,
    /** @brief Daemon to Host CLI: Comprehensive subsystem status report. */
    DaemonMsgStatusResp       = 0x0006,
    /** @brief Host CLI to Daemon: Force kill all subsystem processes. */
    DaemonMsgKillReq          = 0x0007,
    /** @brief Daemon to Host CLI: Result of kill request. */
    DaemonMsgKillResp         = 0x0008,
    /** @brief Host CLI to Daemon: Query active VirtIO-FS shared mounts. */
    DaemonMsgFsListReq        = 0x0009,
    /** @brief Daemon to Host CLI: Active VirtIO-FS shared mounts list. */
    DaemonMsgFsListResp       = 0x000A,
    /** @brief Host CLI to Daemon: Query log records. */
    DaemonMsgLogsReq          = 0x000B,
    /** @brief Daemon to Host CLI: Log output payload. */
    DaemonMsgLogsResp         = 0x000C,
    /** @brief Daemon to Host CLI: Generic error response frame. */
    /** @brief Request idle supervisor exit; empty payload, EBUSY with live children. */
    DaemonMsgShutdownReq      = 0x000D,
    /** @brief Shutdown response using waddle_daemon_result_resp_t before disconnect. */
    DaemonMsgShutdownResp     = 0x000E,
    /** @brief ACPI-only stop; eight-byte stop payload with force=0, StopResp reply.
     * Timeout returns ETIMEDOUT without killing children or releasing mappings.
     */
    DaemonMsgStopGracefulReq   = 0x000F,
    DaemonMsgErrorResp        = 0x00FF
} waddle_daemon_msg_type_t;

/**
 * @brief Subsystem operational state machine states.
 */
typedef enum waddle_subsystem_state_t {
    /** @brief Subsystem is completely stopped; no processes running. */
    SubsystemStateStopped          = 0,
    /** @brief Daemon is spawning and verifying virtiofsd vhost-user socket. */
    SubsystemStateStartingVirtiofs = 1,
    /** @brief Daemon is launching QEMU and completing QMP handshake. */
    SubsystemStateStartingQemu     = 2,
    /** @brief Daemon is waiting for guest agent readiness over VSOCK. */
    SubsystemStateWaitingGuest     = 3,
    /** @brief Subsystem is fully operational; guest agent is ready. */
    SubsystemStateRunning          = 4,
    /** @brief Subsystem is executing graceful shutdown via QMP system_powerdown. */
    SubsystemStateStopping         = 5,
    /** @brief Subsystem encountered a fatal initialization or runtime error. */
    SubsystemStateFailed           = 6
} waddle_subsystem_state_t;

/**
 * @brief Start request configuration flags.
 */
typedef enum waddle_daemon_start_flags_t {
    /** @brief Default: synchronously wait for guest agent readiness. */
    DaemonStartFlagWaitGuest       = 0x0001,
    /** @brief Spawn hypervisor in headless mode without graphics display. */
    DaemonStartFlagHeadless        = 0x0002
} waddle_daemon_start_flags_t;

/**
 * @brief Common 16-byte header prepended to every daemon control message.
 */
typedef struct waddle_daemon_header_t {
    /** @brief Magic number: WaddleDaemonMagic (0x57414444). */
    uint32_t magic;
    /** @brief Protocol version: WaddleDaemonVersion (1). */
    uint16_t version;
    /** @brief Message type enum (waddle_daemon_msg_type_t). */
    uint16_t msg_type;
    /** @brief Client-assigned request/response correlation sequence ID. */
    uint32_t sequence;
    /** @brief Length of trailing payload in bytes (must not exceed WaddleDaemonMaxPayloadSize). */
    uint32_t payload_len;
} waddle_daemon_header_t;

/**
 * @brief Payload for DaemonMsgStartReq.
 */
typedef struct waddle_daemon_start_req_t {
    /** @brief Bitmask of waddle_daemon_start_flags_t flags. */
    uint32_t flags;
    /** @brief Maximum wait time in seconds (0 = default WaddleDefaultStartTimeoutSec). */
    uint32_t timeout_sec;
} waddle_daemon_start_req_t;

/**
 * @brief Payload for DaemonMsgStartResp, DaemonMsgStopResp, DaemonMsgKillResp, DaemonMsgErrorResp.
 */
typedef struct waddle_daemon_result_resp_t {
    /** @brief 0 on success, or non-zero POSIX errno / error code on failure. */
    uint32_t status_code;
    /** @brief Current operational state (waddle_subsystem_state_t). */
    uint32_t subsystem_state;
    /** @brief Human-readable diagnostic error message (NUL-terminated). */
    char error_msg[256];
} waddle_daemon_result_resp_t;

/**
 * @brief Payload for DaemonMsgStopReq.
 */
typedef struct waddle_daemon_stop_req_t {
    /** @brief 1 for immediate termination (SIGKILL), 0 for graceful ACPI shutdown. */
    uint32_t force;
    /** @brief Timeout in seconds before force kill fallback (0 = default 15s). */
    uint32_t timeout_sec;
} waddle_daemon_stop_req_t;

/**
 * @brief Payload for DaemonMsgKillReq.
 */
typedef struct waddle_daemon_kill_req_t {
    /** @brief Flags reserved for future selective kill options. */
    uint32_t flags;
    /** @brief Reserved for 8-byte alignment. */
    uint32_t reserved;
} waddle_daemon_kill_req_t;

/**
 * @brief Filesystem export mapping descriptor.
 */
typedef struct waddle_daemon_fs_mount_t {
    /** @brief Host export directory path (e.g. "/home/dev"). NUL-terminated. */
    char host_path[WaddleMaxPathLen];
    /** @brief Guest drive letter or UNC tag (e.g. "Z:\\"). NUL-terminated. */
    char guest_drive[32];
    /** @brief 1 if read-only, 0 for read-write. */
    uint32_t read_only;
    /** @brief Reserved padding for 8-byte alignment. */
    uint32_t reserved;
} waddle_daemon_fs_mount_t;

/**
 * @brief Payload for DaemonMsgStatusResp.
 */
typedef struct waddle_daemon_status_resp_t {
    /** @brief Current operational state (waddle_subsystem_state_t). */
    uint32_t subsystem_state;
    /** @brief Process ID of the waddled supervisor (0 if stopped). */
    uint32_t daemon_pid;
    /** @brief Process ID of qemu-system-x86_64 (0 if not running). */
    uint32_t qemu_pid;
    /** @brief Process ID of virtiofsd (0 if not running). */
    uint32_t virtiofsd_pid;
    /** @brief VSOCK CID assigned to guest (default 3). */
    uint32_t vsock_cid;
    /** @brief VSOCK port of guest execution agent (default 5242). */
    uint32_t vsock_port;
    /** @brief Seconds elapsed since subsystem transitioned to Running state. */
    uint64_t uptime_sec;
    /** @brief Configured guest RAM in megabytes. */
    uint32_t memory_mb;
    /** @brief Configured guest vCPU count. */
    uint32_t vcpus;
    /** @brief Number of active VirtIO-FS mount descriptors in mounts array. */
    uint32_t mount_count;
    /** @brief Reserved padding for 8-byte alignment. */
    uint32_t reserved;
    /** @brief Array of active export mount mappings. */
    waddle_daemon_fs_mount_t mounts[WaddleMaxMounts];
} waddle_daemon_status_resp_t;

/**
 * @brief Payload for DaemonMsgFsListResp.
 */
typedef struct waddle_daemon_fs_list_resp_t {
    /** @brief Number of active mounts in the following array. */
    uint32_t mount_count;
    /** @brief Reserved padding for 8-byte alignment. */
    uint32_t reserved;
    /** @brief Array of active export mount mappings. */
    waddle_daemon_fs_mount_t mounts[WaddleMaxMounts];
} waddle_daemon_fs_list_resp_t;

/**
 * @brief Payload for DaemonMsgLogsReq.
 */
typedef struct waddle_daemon_logs_req_t {
    /** @brief Target log stream: 0 = all, 1 = daemon, 2 = QEMU, 3 = virtiofsd. */
    uint32_t target;
    /** @brief Number of trailing lines to return (0 = all or default 100). */
    uint32_t lines;
    /** @brief Non-zero if client requests continuous follow streaming. */
    uint32_t follow;
    /** @brief Reserved padding for 8-byte alignment. */
    uint32_t reserved;
} waddle_daemon_logs_req_t;

/**
 * @brief Validates a daemon message header.
 *
 * Verifies that the header magic matches WaddleDaemonMagic, the protocol version
 * matches WaddleDaemonVersion, and payload length does not exceed WaddleDaemonMaxPayloadSize.
 *
 * @param[in] hdr Non-null pointer to waddle_daemon_header_t to validate.
 * @return 0 if valid, or -1 if invalid (magic mismatch, unsupported version, or oversize payload).
 */
int waddle_daemon_header_validate(const waddle_daemon_header_t *hdr);

/**
 * @brief Formats a human-readable string representation of a subsystem operational state.
 *
 * @param[in] state Operational state from waddle_subsystem_state_t.
 * @return Static NUL-terminated string describing the state.
 */
const char *waddle_subsystem_state_to_string(waddle_subsystem_state_t state);

/**
 * @brief Formats a human-readable string representation of a daemon message type.
 *
 * @param[in] type Message type from waddle_daemon_msg_type_t.
 * @return Static NUL-terminated string describing the message type.
 */
const char *waddle_daemon_msg_type_to_string(waddle_daemon_msg_type_t type);

/**
 * @brief Sends a complete daemon message frame over a socket.
 *
 * Transmits the 16-byte header followed by the optional payload bytes.
 *
 * @param[in] fd          Connected UNIX domain socket descriptor.
 * @param[in] msg_type    Message type enum (waddle_daemon_msg_type_t).
 * @param[in] sequence    Request sequence correlation ID.
 * @param[in] payload     Pointer to payload buffer, or NULL if payload_len is 0.
 * @param[in] payload_len Length of payload in bytes (must not exceed WaddleDaemonMaxPayloadSize).
 * @return 0 on success, or -1 on I/O or validation failure (errno set).
 */
int waddle_daemon_send_msg(int fd,
                           uint16_t msg_type,
                           uint32_t sequence,
                           const void *payload,
                           uint32_t payload_len);

/**
 * @brief Reads a complete daemon message frame from a socket within a deadline.
 *
 * Reads and validates the 16-byte header, then reads the trailing payload into
 * the destination buffer.
 *
 * @param[in]     fd              Connected socket descriptor.
 * @param[out]    hdr             Destination pointer for parsed header.
 * @param[out]    payload_buf     Destination buffer for payload bytes, or NULL if payload_buf_len is 0.
 * @param[in]     payload_buf_len Capacity of payload_buf in bytes.
 * @param[in]     timeout_ms      Timeout in milliseconds (0 for indefinite block).
 * @return 0 on success, -1 on I/O or protocol error, or -2 on peer disconnect/EOF before header.
 */
int waddle_daemon_recv_msg(int fd,
                           waddle_daemon_header_t *hdr,
                           void *payload_buf,
                           uint32_t payload_buf_len,
                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_PROTOCOL_H */
