#ifndef WaddleAvPeerH
#define WaddleAvPeerH
#include "waddle/av_protocol.h"
/** @brief Bounded queued control frames, enough for lifecycle burst; immutable. */
#define AvPeerQueueFrames 128u
/** @brief Nonblocking single-event-thread control peer; no owned descriptors/heap. */
typedef struct av_peer_t {
    uintptr_t socket; /**< Borrowed native nonblocking socket, retained until teardown. */
    uint8_t incoming[AvControlBytes]; /**< Partial frame bytes, private to event thread. */
    size_t received;                  /**< Partial receive offset, 0..AvControlBytes-1. */
    uint8_t outgoing[AvPeerQueueFrames][AvControlBytes]; /**< Bounded FIFO frames. */
    uint32_t head; /**< Event-thread enqueue cursor, modulo uint32. */
    uint32_t tail; /**< Event-thread send cursor, modulo uint32. */
    size_t sent;   /**< Partial send offset for oldest frame. */
} av_peer_t;
/** @brief Borrowed transient decoded message callback, event thread only.
 * @param[in] message Nonnull decoded private message valid only during callback.
 * @param[in,out] context Optional caller-owned context.
 * @return 0 accepted, -1 closes the session. May enqueue on same peer.
 */
typedef int (*av_peer_notify_t)(const av_message_t *message, void *context);
/** @brief Queue one encoded control frame without blocking.
 * @param[in,out] peer Nonnull initialized event-thread-owned peer.
 * @param[in] message Nonnull validated message, copied before return.
 * @return 0 queued, 1 full, -1 invalid. Caller must cancel unsent video slots.
 * @note No allocation/ownership transfer; exactly one event thread.
 */
int av_peer_send(av_peer_t *peer, const av_message_t *message);
/** @brief Pump bounded nonblocking sends/receives and decode complete frames.
 * @param[in,out] peer Nonnull event-thread-owned peer, caller-owned socket.
 * @param[in] notify Nonnull callback retained only during call.
 * @param[in,out] context Optional caller-owned context.
 * @return 0 alive/would-block, -1 EOF/socket/codec/callback failure.
 * @note At most 16 receive frames and 16 send attempts per call; no allocation,
 * no spinning on EAGAIN. Use poll/select between calls. Socket remains caller-owned.
 */
int av_peer_pump(av_peer_t *peer, av_peer_notify_t notify, void *context);
#endif
