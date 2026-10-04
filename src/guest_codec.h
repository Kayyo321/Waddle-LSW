/** @file guest_codec.h @brief Allocation-free, bounds-checked guest wire ABI. */
#ifndef WaddleGuestCodecH
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
#endif
