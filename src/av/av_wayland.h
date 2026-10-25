#ifndef WaddleAvWaylandH
#define WaddleAvWaylandH
#include "av_transport.h"
#include "waddle/av_protocol.h"
#include <wayland-client.h>
/** @brief Opaque caller-owned host video client, created/freed on one event thread. */
typedef struct av_wayland_t av_wayland_t;
/** @brief Borrowed synchronous host request callback, no ownership transfer.
 * @param[in] request Nonnull transient validated geometry/close or physical-mode input request.
 * @param[in,out] context Optional retained caller context.
 * @return 0 on queued request, -1 on delivery failure.
 * @note Wayland event thread; may not retain request or destroy client recursively.
 */
typedef int (*av_host_request_t)(const av_message_t *request, void *context);
/** @brief Connect to default Wayland display and bind compositor/shm/xdg-shell and optional version-5 seat.
 * @param[out] client Nonnull result; set NULL on failure; owned until free.
 * @param[in] request Nonnull borrowed callback retained until free.
 * @param[in,out] context Optional caller-owned callback context.
 * @return 0 on success, -1 on connection/global/allocation failure.
 * @note Event thread only, all allocation owned by av_wayland_free. Missing seat
 * or any legacy Create leaves video display-only, including resize/close. V3
 * input requires a completed lease barrier and fresh keyboard enter. Keyboard is physical
 * PC scan forwarding interpreted with the guest layout; no XKB text/IME/repeat.
 */
int av_wayland_init(av_wayland_t **client, av_host_request_t request, void *context);
/** @brief Create a captured-window toplevel backed by three shared pixel regions.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @param[in] message Nonnull validated MsgWindowCreate, MsgWindowCreateV2 or MsgWindowCreateV3 metadata.
 * @param[in] fd Borrowed shared-memory file FD, valid through free.
 * @param[in] mapping_size Accessible shared bytes, 1..INT32_MAX for wl_shm.
 * @param[in] slots Three nonnull aligned slot pointers retained through free.
 * @param[in] offsets Three pixel offsets within fd, retained only during call.
 * @param[in] capacity Byte capacity per pixel region; all fit mapping_size.
 * @return 0 on creation, -1 on duplicate/id/registry/global/resource failure.
 * @note Event thread only; caller retains FD and mapping until client free.
 */
int av_wayland_create(av_wayland_t *client, const av_message_t *message, int fd,
                      size_t mapping_size, window_slot_header_t *slots[3],
                      const uint64_t offsets[3], size_t capacity);
/** @brief Apply guest lease controls or lifecycle/geometry/frame for a toplevel.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @param[in] message Nonnull validated destroy/geometry/frame or guest lease control.
 * @return 0 accepted/stale lifecycle/drop-busy frame, -1 malformed/session/resource failure.
 * @note Event thread only; lease controls route before window/buffer lookup.
 * Frames release ownership only on wl_buffer.release.
 */
int av_wayland_message(av_wayland_t *client, const av_message_t *message);
/** @brief Dispatch Wayland events after the display FD became readable.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @return 0 on success, -1 on terminal failure. May invoke the request callback.
 * @note Event thread only. Never blocks: it runs av_wayland_timeout(0), which drains the
 * socket without blocking before it services repeats.
 */
int av_wayland_dispatch(av_wayland_t *client);
/** @brief Drain the socket without blocking, service due input repeats, check the V3
 * handshake deadline, and bound the next event-loop wait.
 * @param[in,out] client Nonnull event-thread-owned initialized instance.
 * @param[in] maximum Nonnegative requested maximum wait in milliseconds.
 * @return 0..maximum safe wait, -1 terminal, clock or deadline failure.
 * @note Order: queued Wayland events are dispatched first, so a release already in the
 * socket cancels a repeat that is due at the same instant. A due repeat is a key down
 * that consumes a serial (it may inject input). Poll timeouts round a repeat's remaining
 * time up to whole milliseconds. EINTR never restarts a deadline.
 */
int av_wayland_timeout(av_wayland_t *client, int maximum);
/** @brief Optional non-failing queue probe for input repeats.
 * @param[in] context The request context passed to av_wayland_init. @return 1 when a frame fits, 0 when full.
 */
typedef int (*av_wayland_room_t)(void *context);
/** @brief Install the queue probe used to drop, rather than fail on, a repeat when the queue is full.
 * @param[in,out] client Nonnull event-thread-owned instance. @param[in] room NULL means room is always available.
 */
void av_wayland_set_room(av_wayland_t *client, av_wayland_room_t room);
/** @brief Diagnostic count of input repeats dropped because the queue was full. @return count since init. */
uint32_t av_wayland_repeats_dropped(const av_wayland_t *client);
/** @brief Flush outgoing protocol bytes without blocking.
 * @param[in,out] client Nonnull event-thread-owned instance.
 * @return 0 flushed/would-block, -1 display error; no ownership transfer.
 * @note Event thread; writable() reports whether POLLOUT must be requested.
 */
int av_wayland_flush(av_wayland_t *client);
/** @brief Query outgoing display backpressure.
 * @param[in] client Nonnull borrowed instance.
 * @return 1 poll POLLOUT, 0 no pending flush; event thread, allocation-free.
 */
int av_wayland_writable(const av_wayland_t *client);
/** @brief Return a borrowed display FD for caller poll integration.
 * @param[in] client Nonnull initialized client.
 * @return Nonnegative borrowed FD; caller must not close it.
 * @note Event thread only; valid until free, no allocation.
 */
int av_wayland_fd(av_wayland_t *client);
/** @brief Disconnect display and release all owned protocol objects/context.
 * @param[in,out] client Nonnull pointer to owned instance, NULL accepted as value.
 * @note Event thread only; sets *client NULL; mapping retained by caller until
 * compositor/guest are quiescent. Retires surfaces and drains releases for at most
 * two seconds, after cancelling lease synchronization and disabling input.
 * Unreleased slots remain consumed on compositor error/timeout.
 */
void av_wayland_free(av_wayland_t **client);
/** @brief Diagnostic commit acknowledgement callback, event thread only.
 * @param[in,out] context Optional borrowed context retained until completion/free.
 * @note No result/ownership transfer; called after compositor processes matching
 * surface commit. Physical presentation/scanout is outside this endpoint.
 */
typedef void (*av_commit_done_t)(void *context);
/** @brief Arm one diagnostic RGB24 match and compositor processing barrier.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @param[in] window_id Nonzero existing window identifier.
 * @param[in] token Nonzero RGB24 expected center 3x3 color.
 * @param[in] mapping Nonnull borrowed shared mapping retained until free.
 * @param[in] length Accessible mapping bytes.
 * @param[in] done Nonnull callback retained until completion/free.
 * @param[in,out] context Optional borrowed callback context.
 * @return 0 armed, -1 invalid/busy. No allocation until matching commit creates
 * a Wayland sync proxy, owned/destroyed by callback or client free.
 */
int av_wayland_watch(av_wayland_t *client, uint64_t window_id, uint32_t token,
                     const uint8_t *mapping, size_t length, av_commit_done_t done, void *context);
#endif
