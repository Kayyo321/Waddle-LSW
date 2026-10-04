/** @file guest_codec.h @brief Allocation-free, bounds-checked guest wire ABI. */
#ifndef WaddleGuestCodecH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddleGuestCodecH
#include <stddef.h>
#include <stdint.h>
/** @brief Validated header values; caller-owned, no pointers or shared state. */
typedef struct guest_frame_t {
    uint32_t length; /**< Payload bytes, at most 1 MiB. */
    uint32_t crc; /**< Expected IEEE CRC-32 of the payload. */
    uint16_t type; /**< Valid host message type. */
} guest_frame_t;
/** @brief Validate a borrowed header before allocating payload storage; thread-safe.
 * @param[in] header Nonnull readable buffer of length bytes.
 * @param[in] length Header buffer length; must be 32.
 * @param[in] sequence Expected host sequence, including unsigned wrap.
 * @param[out] frame Nonnull caller-owned result, written only on success.
 * @return 0 success, -1 malformed input; no allocation or ownership transfer. */
int guest_header_validate(const uint8_t *header, size_t length, uint32_t sequence,
                          guest_frame_t *frame);
/** @brief Compute CRC of borrowed bytes, without allocation; thread-safe.
 * @param[in] data Nonnull readable length-byte buffer (may be empty).
 * @param[in] length Buffer byte count.
 * @return IEEE CRC-32; no ownership transfer or errors. */
uint32_t guest_crc32(const uint8_t *data, size_t length);
/** @brief Borrowed spawn views, valid until the input frame is released; immutable.
 * @note No allocations; caller owns this result and serializes mutation. */
typedef struct guest_spawn_t {
    const uint8_t *cwd; /**< Borrowed NUL-terminated UTF-8 working directory. */
    const uint8_t *command; /**< Borrowed canonical UTF-8 command line. */
    const uint8_t *environment; /**< Borrowed NUL-delimited KEY=VALUE entries. */
    uint32_t cwd_length; /**< Working directory bytes excluding NUL. */
    uint32_t command_length; /**< Command bytes excluding NUL. */
    uint32_t environment_length; /**< Environment bytes including entry terminators. */
    uint16_t rows; /**< Positive signed-16-bit terminal rows in interactive mode. */
    uint16_t cols; /**< Positive signed-16-bit terminal columns in interactive mode. */
    uint32_t interactive; /**< One for ConPTY, zero for raw pipes. */
} guest_spawn_t;
/** @brief Validate an untrusted spawn frame without allocation; thread-safe.
 * @param[in] body Nonnull borrowed length-byte buffer, retained through use of spawn.
 * @param[in] length Payload byte count, at most 1 MiB.
 * @param[out] spawn Nonnull caller-owned views; unspecified on failure.
 * @param[out] executable Nonnull writable caller-owned buffer, disjoint from body.
 * @param[in] capacity Executable buffer bytes including terminator.
 * @return 0 success, -1 malformed input or insufficient capacity. */
int guest_spawn_validate(const uint8_t *body, size_t length, guest_spawn_t *spawn,
                         uint8_t *executable, size_t capacity);
/** @brief Validate a post-spawn host control/data frame; no allocation, thread-safe.
 * @param[in] type Previously validated host frame type.
 * @param[in] body Nonnull borrowed length-byte payload.
 * @param[in] length Payload byte count.
 * @param[in] interactive One for ConPTY, zero for raw pipes.
 * @param[in] input_eof One if stdin EOF was already received.
 * @return 0 valid, -1 invalid type/state/reserved bytes/length/signal/dimensions.
 * @note No state mutation; caller records stdin EOF only after success. */
int guest_control_validate(uint16_t type, const uint8_t *body, size_t length,
                           int interactive, int input_eof);
/** @brief Parse a bounded listener port; allocation-free and thread-safe.
 * @param[in] text Nonnull borrowed NUL-terminated decimal ASCII.
 * @param[out] port Nonnull caller-owned result, unchanged on failure.
 * @return 0 valid 1..UINT32_MAX, -1 invalid/overflow. */
int guest_port_parse(const char *text, uint32_t *port);
#endif
