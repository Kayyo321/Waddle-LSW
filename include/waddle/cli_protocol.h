/**
 * @file cli_protocol.h
 * @brief Wire protocol, data structures, and serialization codecs for CLI passthrough.
 *
 * This header defines the C ABI wire structures, message types, stream identifiers,
 * execution flags, and serialization helpers bridging the host Linux CLI client and
 * the guest Windows execution agent.
 */

#ifndef WADDLE_CLI_PROTOCOL_H
#define WADDLE_CLI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Magic identifier for Waddle CLI wire frames ("WDLC" in ASCII).
 */
#define WaddleCliMagic UINT32_C(0x57444c43)

/**
 * @brief Current protocol specification version.
 */
#define WaddleCliVersion 1

/**
 * @brief Default VSOCK port used for the guest CLI execution agent.
 */
#define WaddleDefaultVsockPort 5242

/**
 * @brief Maximum single payload size in bytes (1 MiB).
 */
#define WaddleMaxPayloadSize (1024u * 1024u)

/**
 * @brief Fixed wire header size in bytes.
 */
#define WaddleHeaderSize 32u

/**
 * @brief Default stream chunk size in bytes (16 KiB).
 */
#define WaddleChunkSize 16384u

/**
 * @brief Maximum buffer capacity for send/receive queues (2 MiB).
 */
#define WaddleQueueSize (2u * 1024u * 1024u)

/* Backward compatibility aliases */
#define WADDLE_CLI_MAGIC WaddleCliMagic
#define WADDLE_CLI_VERSION WaddleCliVersion
#define WADDLE_DEFAULT_VSOCK_PORT WaddleDefaultVsockPort
#define WADDLE_MAX_PAYLOAD_SIZE WaddleMaxPayloadSize
#define WADDLE_HEADER_SIZE WaddleHeaderSize
#define WADDLE_CHUNK_SIZE WaddleChunkSize
#define WADDLE_QUEUE_SIZE WaddleQueueSize

/**
 * @brief Message type identifiers transmitted in the wire header.
 */
typedef enum waddle_cli_msg_type_t {
    /** @brief Host to Guest: Request process execution. */
    WaddleMsgSpawnReq = 1,
    /** @brief Guest to Host: Confirmation or error resulting from process spawn. */
    WaddleMsgSpawnResp = 2,
    /** @brief Bidirectional: Multiplexed standard I/O data payload. */
    WaddleMsgStreamData = 3,
    /** @brief Host to Guest: Terminal window dimensions resize event. */
    WaddleMsgTerminalResize = 4,
    /** @brief Host to Guest: Forwarded signal (e.g. SIGINT, SIGTERM). */
    WaddleMsgSignalEvent = 5,
    /** @brief Guest to Host: Process exit code and termination status. */
    WaddleMsgProcessExit = 6,
    /** @brief Bidirectional: Keepalive heartbeat ping. */
    WaddleMsgHeartbeatPing = 7,
    /** @brief Bidirectional: Keepalive heartbeat pong. */
    WaddleMsgHeartbeatPong = 8,
    /** @brief Bidirectional: End-of-file notification on a stream. */
    WaddleMsgStreamEof = 9,
    /** @brief Bidirectional: Fatal protocol error. */
    WaddleMsgError = 255
} waddle_cli_msg_type_t;

/* Backward compatibility aliases */
#define WADDLE_MSG_SPAWN_REQ WaddleMsgSpawnReq
#define WADDLE_MSG_SPAWN_RESP WaddleMsgSpawnResp
#define WADDLE_MSG_STREAM_DATA WaddleMsgStreamData
#define WADDLE_MSG_TERMINAL_RESIZE WaddleMsgTerminalResize
#define WADDLE_MSG_SIGNAL_EVENT WaddleMsgSignalEvent
#define WADDLE_MSG_PROCESS_EXIT WaddleMsgProcessExit
#define WADDLE_MSG_HEARTBEAT_PING WaddleMsgHeartbeatPing
#define WADDLE_MSG_HEARTBEAT_PONG WaddleMsgHeartbeatPong
#define WADDLE_MSG_STREAM_EOF WaddleMsgStreamEof
#define WADDLE_MSG_ERROR WaddleMsgError

/**
 * @brief Stream identifiers for multiplexed standard I/O streams.
 */
typedef enum waddle_stream_id_t {
    /** @brief Standard input stream (Host to Guest). */
    WaddleStreamStdin = 0,
    /** @brief Standard output stream (Guest to Host). */
    WaddleStreamStdout = 1,
    /** @brief Standard error stream (Guest to Host). */
    WaddleStreamStderr = 2
} waddle_stream_id_t;

/* Backward compatibility aliases */
#define WADDLE_STREAM_STDIN WaddleStreamStdin
#define WADDLE_STREAM_STDOUT WaddleStreamStdout
#define WADDLE_STREAM_STDERR WaddleStreamStderr

/**
 * @brief Process execution and environment configuration flags.
 */
typedef enum waddle_spawn_flags_t {
    /** @brief Allocate an interactive ConPTY pseudo console. */
    WaddleSpawnFlagInteractive = 1,
    /** @brief Allocate raw Win32 anonymous pipes (non-interactive). */
    WaddleSpawnFlagRawPipes = 2,
    /** @brief Merge host environment into child environment. */
    WaddleSpawnFlagInheritEnv = 4,
    /** @brief Automatically translate Linux file paths to guest drive paths. */
    WaddleSpawnFlagTranslatePath = 8,
    /** @brief Request elevated administrator execution. */
    WaddleSpawnFlagElevated = 16
} waddle_spawn_flags_t;

/* Backward compatibility aliases */
#define WADDLE_SPAWN_FLAG_INTERACTIVE WaddleSpawnFlagInteractive
#define WADDLE_SPAWN_FLAG_RAW_PIPES WaddleSpawnFlagRawPipes
#define WADDLE_SPAWN_FLAG_INHERIT_ENV WaddleSpawnFlagInheritEnv
#define WADDLE_SPAWN_FLAG_TRANSLATE_PATH WaddleSpawnFlagTranslatePath
#define WADDLE_SPAWN_FLAG_ELEVATED WaddleSpawnFlagElevated

#pragma pack(push, 1)

/**
 * @brief Standard 32-byte wire header prepended to every protocol frame.
 */
typedef struct waddle_cli_msg_header_t {
    /** @brief Magic identifier (must equal WaddleCliMagic). */
    uint32_t magic;
    /** @brief Protocol specification version (WaddleCliVersion). */
    uint16_t version;
    /** @brief Message type identifier (from waddle_cli_msg_type_t). */
    uint16_t msg_type;
    /** @brief Session identifier unique to this execution instance. */
    uint64_t session_id;
    /** @brief Byte length of payload data immediately following this header. */
    uint32_t payload_len;
    /** @brief Reserved message-specific flags. */
    uint32_t flags;
    /** @brief Monotonically increasing sequence number. */
    uint32_t sequence;
    /** @brief CRC-32 checksum of the payload data. */
    uint32_t crc32;
} waddle_cli_msg_header_t;

/**
 * @brief Fixed payload layout for WaddleMsgSpawnReq messages.
 */
typedef struct waddle_msg_spawn_req_t {
    /** @brief Spawn configuration bitmask (waddle_spawn_flags_t). */
    uint32_t spawn_flags;
    /** @brief Initial terminal row count. */
    uint16_t initial_rows;
    /** @brief Initial terminal column count. */
    uint16_t initial_cols;
    /** @brief Horizontal pixel count (0 if unknown). */
    uint16_t x_pixels;
    /** @brief Vertical pixel count (0 if unknown). */
    uint16_t y_pixels;
    /** @brief Length of working directory string in bytes excluding null terminator. */
    uint32_t cwd_len;
    /** @brief Length of command line string in bytes excluding null terminator. */
    uint32_t cmdline_len;
    /** @brief Length of null-delimited environment variable block in bytes. */
    uint32_t env_len;
} waddle_msg_spawn_req_t;

/**
 * @brief Fixed payload layout for WaddleMsgSpawnResp messages.
 */
typedef struct waddle_msg_spawn_resp_t {
    /** @brief Status code (0 for success, non-zero for system error). */
    uint32_t status_code;
    /** @brief Process ID of the spawned guest child process. */
    uint32_t guest_pid;
    /** @brief Length of optional UTF-8 error description string. */
    uint32_t error_len;
} waddle_msg_spawn_resp_t;

/**
 * @brief Fixed payload layout for WaddleMsgStreamData messages.
 */
typedef struct waddle_msg_stream_data_t {
    /** @brief Stream identifier (from waddle_stream_id_t). */
    uint8_t stream_id;
    /** @brief Explicit padding to 4-byte boundary. */
    uint8_t reserved[3];
    /** @brief Byte length of stream data following this structure. */
    uint32_t data_len;
} waddle_msg_stream_data_t;

/**
 * @brief Payload layout for WaddleMsgTerminalResize messages.
 */
typedef struct waddle_msg_resize_t {
    /** @brief New terminal row count. */
    uint16_t rows;
    /** @brief New terminal column count. */
    uint16_t cols;
    /** @brief Horizontal pixel count (0 if unknown). */
    uint16_t x_pixels;
    /** @brief Vertical pixel count (0 if unknown). */
    uint16_t y_pixels;
} waddle_msg_resize_t;

/**
 * @brief Payload layout for WaddleMsgSignalEvent messages.
 */
typedef struct waddle_msg_signal_t {
    /** @brief POSIX signal number or Windows console event identifier. */
    uint32_t signal_type;
} waddle_msg_signal_t;

/**
 * @brief Payload layout for WaddleMsgProcessExit messages.
 */
typedef struct waddle_msg_exit_t {
    /** @brief Process exit code returned by the child process. */
    uint32_t exit_code;
    /** @brief Termination status (0 = clean exit, 1 = signaled). */
    uint32_t termination_status;
    /** @brief Wall-clock execution time of child process in milliseconds. */
    uint64_t wall_time_ms;
} waddle_msg_exit_t;

#pragma pack(pop)

_Static_assert(sizeof(waddle_cli_msg_header_t) == 32, "wire header size must be 32 bytes");
_Static_assert(sizeof(waddle_msg_spawn_req_t) == 24, "spawn req size must be 24 bytes");
_Static_assert(sizeof(waddle_msg_spawn_resp_t) == 12, "spawn resp size must be 12 bytes");
_Static_assert(sizeof(waddle_msg_stream_data_t) == 8, "stream data size must be 8 bytes");
_Static_assert(sizeof(waddle_msg_resize_t) == 8, "resize size must be 8 bytes");
_Static_assert(sizeof(waddle_msg_signal_t) == 4, "signal size must be 4 bytes");
_Static_assert(sizeof(waddle_msg_exit_t) == 16, "exit size must be 16 bytes");

/**
 * @brief Decodes an unaligned 16-bit little-endian integer from a byte buffer.
 *
 * @param[in] p Non-null pointer to at least 2 readable bytes.
 * @return Decoded 16-bit unsigned integer.
 * @note Thread-safe; performs no memory allocations.
 */
uint16_t waddle_get16(const void *p);

/**
 * @brief Decodes an unaligned 32-bit little-endian integer from a byte buffer.
 *
 * @param[in] p Non-null pointer to at least 4 readable bytes.
 * @return Decoded 32-bit unsigned integer.
 * @note Thread-safe; performs no memory allocations.
 */
uint32_t waddle_get32(const void *p);

/**
 * @brief Decodes an unaligned 64-bit little-endian integer from a byte buffer.
 *
 * @param[in] p Non-null pointer to at least 8 readable bytes.
 * @return Decoded 64-bit unsigned integer.
 * @note Thread-safe; performs no memory allocations.
 */
uint64_t waddle_get64(const void *p);

/**
 * @brief Encodes a 16-bit unsigned integer in little-endian format into a byte buffer.
 *
 * @param[out] p Non-null pointer to at least 2 writable bytes.
 * @param[in]  n 16-bit integer value to write.
 * @note Thread-safe; performs no memory allocations.
 */
void waddle_put16(void *p, uint16_t n);

/**
 * @brief Encodes a 32-bit unsigned integer in little-endian format into a byte buffer.
 *
 * @param[out] p Non-null pointer to at least 4 writable bytes.
 * @param[in]  n 32-bit integer value to write.
 * @note Thread-safe; performs no memory allocations.
 */
void waddle_put32(void *p, uint32_t n);

/**
 * @brief Encodes a 64-bit unsigned integer in little-endian format into a byte buffer.
 *
 * @param[out] p Non-null pointer to at least 8 writable bytes.
 * @param[in]  n 64-bit integer value to write.
 * @note Thread-safe; performs no memory allocations.
 */
void waddle_put64(void *p, uint64_t n);

/**
 * @brief Computes standard IEEE 802.3 CRC-32 checksum across a contiguous memory buffer.
 *
 * @param[in] p Buffer to checksum. May be NULL if n is 0.
 * @param[in] n Byte length of buffer.
 * @return Computed 32-bit CRC checksum value.
 * @note Thread-safe; performs no memory allocations.
 */
uint32_t waddle_crc32(const void *p, size_t n);

/**
 * @brief Quotes a NULL-terminated array of arguments into a single Win32 command line string.
 *
 * Escapes special characters, embedded quotes, and backslashes according to the inverse
 * specification of CommandLineToArgvW.
 *
 * @param[in] argv NULL-terminated array of non-null argument strings.
 * @return Dynamically allocated null-terminated command line string, or NULL on error.
 * @note Caller takes ownership of the returned pointer and must free it via free().
 * @note Sets errno to E2BIG if formatted command exceeds WaddleMaxPayloadSize.
 */
char *waddle_quote(char *const argv[]);

/**
 * @brief Parses a quoted Win32 command line string into an array of individual argument tokens.
 *
 * @param[in] command Non-null null-terminated Win32 command line string.
 * @return Dynamically allocated NULL-terminated array of allocated strings, or NULL on error.
 * @note Caller takes ownership of returned array and must deallocate it via waddle_free_argv().
 * @note Sets errno to EINVAL on malformed quoting or unescaped characters.
 */
char **waddle_unquote(const char *command);

/**
 * @brief Deallocates an argument array returned by waddle_unquote.
 *
 * Defensively frees all inner argument strings, frees the outer array pointer, and leaves
 * no leaked allocations.
 *
 * @param[in,out] argv Array of strings to free. Safe to call with NULL.
 * @note Parameter memory is freed deterministically.
 */
void waddle_free_argv(char **argv);

/**
 * @brief Translates a Linux host filesystem path to a guest Windows VirtIO-FS mapped drive path.
 *
 * Transforms absolute POSIX paths (e.g. `/home/dev`) into mapped Windows drive paths
 * (e.g. `Z:\home\dev`), replacing forward slashes with backslashes. Relative paths are preserved.
 *
 * @param[in] path Non-null null-terminated POSIX path string.
 * @return Dynamically allocated translated path string, or NULL on error.
 * @note Caller takes ownership of the returned pointer and must free it via free().
 * @note Sets errno to EINVAL if path contains path traversal segments (`..`) or invalid characters.
 */
char *waddle_translate_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_CLI_PROTOCOL_H */
