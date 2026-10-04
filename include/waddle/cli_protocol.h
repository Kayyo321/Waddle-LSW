#ifndef WADDLE_CLI_PROTOCOL_H
#define WADDLE_CLI_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define WADDLE_CLI_MAGIC UINT32_C(0x57444c43)
#define WADDLE_CLI_VERSION 1
#define WADDLE_DEFAULT_VSOCK_PORT 5242
#define WADDLE_MAX_PAYLOAD_SIZE (1024u * 1024u)
#define WADDLE_HEADER_SIZE 32u
#define WADDLE_CHUNK_SIZE 16384u
#define WADDLE_QUEUE_SIZE (2u * 1024u * 1024u)
enum waddle_cli_msg_type {
    WADDLE_MSG_SPAWN_REQ = 1, WADDLE_MSG_SPAWN_RESP,
    WADDLE_MSG_STREAM_DATA, WADDLE_MSG_TERMINAL_RESIZE,
    WADDLE_MSG_SIGNAL_EVENT, WADDLE_MSG_PROCESS_EXIT,
    WADDLE_MSG_HEARTBEAT_PING, WADDLE_MSG_HEARTBEAT_PONG,
    WADDLE_MSG_STREAM_EOF, WADDLE_MSG_ERROR = 255
};
enum waddle_stream_id { WADDLE_STREAM_STDIN, WADDLE_STREAM_STDOUT, WADDLE_STREAM_STDERR };
enum waddle_spawn_flags {
    WADDLE_SPAWN_FLAG_INTERACTIVE = 1, WADDLE_SPAWN_FLAG_RAW_PIPES = 2,
    WADDLE_SPAWN_FLAG_INHERIT_ENV = 4, WADDLE_SPAWN_FLAG_TRANSLATE_PATH = 8,
    WADDLE_SPAWN_FLAG_ELEVATED = 16
};
#pragma pack(push, 1)
typedef struct {
    uint32_t magic; uint16_t version, msg_type; uint64_t session_id;
    uint32_t payload_len, flags, sequence, crc32;
} waddle_cli_msg_header_t;
typedef struct {
    uint32_t spawn_flags; uint16_t initial_rows, initial_cols, x_pixels, y_pixels;
    uint32_t cwd_len, cmdline_len, env_len;
} waddle_msg_spawn_req_t;
typedef struct { uint32_t status_code, guest_pid, error_len; } waddle_msg_spawn_resp_t;
typedef struct { uint8_t stream_id, reserved[3]; uint32_t data_len; } waddle_msg_stream_data_t;
typedef struct { uint16_t rows, cols, x_pixels, y_pixels; } waddle_msg_resize_t;
typedef struct { uint32_t signal_type; } waddle_msg_signal_t;
typedef struct { uint32_t exit_code, termination_status; uint64_t wall_time_ms; } waddle_msg_exit_t;
#pragma pack(pop)
_Static_assert(sizeof(waddle_cli_msg_header_t) == 32, "wire header size");
_Static_assert(sizeof(waddle_msg_spawn_req_t) == 24, "spawn size");
_Static_assert(sizeof(waddle_msg_spawn_resp_t) == 12, "response size");
_Static_assert(sizeof(waddle_msg_stream_data_t) == 8, "stream size");
_Static_assert(sizeof(waddle_msg_resize_t) == 8, "resize size");
_Static_assert(sizeof(waddle_msg_signal_t) == 4, "signal size");
_Static_assert(sizeof(waddle_msg_exit_t) == 16, "exit size");
uint16_t waddle_get16(const void *p);
uint32_t waddle_get32(const void *p);
uint64_t waddle_get64(const void *p);
void waddle_put16(void *p, uint16_t n);
void waddle_put32(void *p, uint32_t n);
void waddle_put64(void *p, uint64_t n);
uint32_t waddle_crc32(const void *p, size_t n);
/* NULL on allocation/size failure. Caller owns returned storage. */
char *waddle_quote(char *const argv[]);
char **waddle_unquote(const char *command);
void waddle_free_argv(char **argv);
char *waddle_translate_path(const char *path);
#endif
