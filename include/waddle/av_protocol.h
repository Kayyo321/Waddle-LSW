#ifndef WaddleAvProtocolH
#define WaddleAvProtocolH
#include <stddef.h>
#include <stdint.h>
/** @brief AV control port; separate from CLI framing. Immutable. */
#define AvControlPort 5001u
/** @brief Fixed control frame bytes including eight-byte header. */
#define AvControlBytes 328u
/** @brief Maximum number of simultaneous application windows. */
#define AvMaxWindows 16u
/** @brief Number of video buffers for each application window. */
#define AvVideoBuffers 3u
/** @brief Guest minimized state bit; immutable. */
#define AvWindowMinimized 1u
/** @brief Guest fullscreen state bit; immutable. */
#define AvWindowFullscreen 2u
/** @brief Private WM_APP fixture flash message; never sent to ordinary windows. */
#define AvDiagnosticFlashEvent 0x8025u
/** @brief AV control message types; IDs scoped to AV connection only. */
typedef enum av_message_type_t {
    /** Guest announces an eligible window. */ MsgWindowCreate = 1,
    /** Guest destroys a previously announced window. */ MsgWindowDestroy = 2,
    /** Either peer updates size/state; position remains guest metadata. */ MsgWindowGeometry = 3,
    /** Guest publishes a triple-buffer slot and one bounded damage rectangle. */ MsgFrameReady = 4,
    /** Host requests WM_CLOSE for the guest window. */ MsgWindowClose = 5,
    /** Host-only opt-in fixture flash; sequence is RGB24 token, flags bit 0 occludes. */ MsgDiagnosticFlash = 6
} av_message_type_t;
/** @brief Decoded control data, caller-owned, natural alignment, no wire casts.
 * @note Codec copies fields using explicit LE offsets; struct is not serialized
 * with memcpy. No allocations; caller retains result. Safe on independent threads.
 */
typedef struct av_message_t {
    uint64_t window_id; /**< Nonzero opaque guest HWND identifier. */
    int32_t x; /**< Signed guest screen coordinate. */
    int32_t y; /**< Signed guest screen coordinate. */
    uint32_t width; /**< Nonzero bounded pixel width except destroy/close. */
    uint32_t height; /**< Nonzero bounded pixel height except destroy/close. */
    uint32_t flags; /**< Minimized/fullscreen only; no unknown bits allowed. */
    uint32_t dpi; /**< Window DPI, 48..768 for create/geometry/frame. */
    uint32_t process_id; /**< Target process ID; nonzero for create. */
    uint32_t buffer_index; /**< Frame slot 0..2. */
    uint64_t sequence; /**< Nonzero frame sequence for FrameReady. */
    int32_t damage_x; /**< Nonnegative surface-local damage origin. */
    int32_t damage_y; /**< Nonnegative surface-local damage origin. */
    uint32_t damage_width; /**< Bounded nonzero damage width for frames. */
    uint32_t damage_height; /**< Bounded nonzero damage height for frames. */
    char title[256]; /**< NUL-terminated UTF-8; unused bytes zeroed by encoder. */
    uint32_t type; /**< av_message_type_t, decoded from header. */
} av_message_t;
/** @brief Decode and validate one fixed-size AV frame.
 * @param[in] bytes Nonnull borrowed input[length], may be untrusted.
 * @param[in] length Accessible bytes; must equal AvControlBytes.
 * @param[out] message Nonnull caller-owned result, unchanged on error.
 * @return 0 on success, -1 on length/header/field corruption.
 * @note Pure/thread-safe; no allocations or ownership transfer.
 */
int av_control_decode(const uint8_t *bytes, size_t length, av_message_t *message);
/** @brief Encode a validated message into canonical little-endian bytes.
 * @param[in] message Nonnull borrowed data retained during call.
 * @param[out] bytes Nonnull caller-owned buffer with capacity bytes.
 * @param[in] capacity Accessible bytes, at least AvControlBytes.
 * @return 0 on success, -1 on invalid message/capacity, output unchanged.
 * @note Pure/thread-safe; no allocation; no overlap between arguments.
 */
int av_control_encode(const av_message_t *message, uint8_t *bytes, size_t capacity);
/** @brief Parse a bounded nonzero decimal process/CID argument.
 * @param[in] bytes Nonnull borrowed input[length], 1..10 decimal bytes.
 * @param[in] length Accessible input bytes, excludes NUL terminator.
 * @param[out] value Nonnull caller-owned result, unchanged on failure.
 * @return 0 success, -1 invalid/zero/overflow. Pure/thread-safe, no allocation.
 */
int av_number_parse(const char *bytes, size_t length, uint32_t *value);
#endif
