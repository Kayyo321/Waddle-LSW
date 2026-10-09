#ifndef WaddleAvPlatformRequestsH
#define WaddleAvPlatformRequestsH
#include "waddle/av_protocol.h"
/** @brief Synthetic native-platform fixture window; no corresponding Windows HWND. */
#define AvPlatformWindow UINT64_C(4242)
/** @brief Immutable incarnation announced by this fixture's CreateV2. */
#define AvPlatformIncarnation UINT64_C(1)
/** @brief Caller-owned fixture counters, zero-initialized for each synthetic client.
 * @note Event-thread-only, no resources or injection state are owned here.
 */
typedef struct av_platform_requests_t {
    unsigned controls; /**< Valid Geometry/Close callbacks recorded. */
    unsigned inputs; /**< Valid physical input callbacks recorded, never injected. */
    uint32_t last_input_serial; /**< Last canonical increasing input serial. */
} av_platform_requests_t;
/** @brief Validate and record a callback for the modern synthetic fixture window.
 * @param[in] message Optional borrowed callback; NULL is rejected.
 * @param[in,out] context Optional borrowed av_platform_requests_t; NULL rejected.
 * @return 0 recorded, -1 unexpected type/identity, malformed fields or replay.
 * @note Event-thread-only, allocation-free; rejection leaves counters unchanged.
 * Never sends, forwards or injects input. Counts do not establish guest behavior.
 */
static inline int av_platform_record_request(const av_message_t *message, void *context) {
    av_platform_requests_t *requests = context;
    uint8_t bytes[AvControlBytes];
    if (!requests || !message || message->window_id != AvPlatformWindow ||
        message->sequence != AvPlatformIncarnation ||
        av_control_encode(message, bytes, sizeof(bytes)) != 0) return -1;
    if (message->type == MsgWindowGeometry || message->type == MsgWindowClose) {
        ++requests->controls;
        return 0;
    }
    if (message->type >= MsgInputFocus && message->type <= MsgInputRelease &&
        message->buffer_index > requests->last_input_serial) {
        requests->last_input_serial = message->buffer_index;
        ++requests->inputs;
        return 0;
    }
    return -1;
}
#endif
