#ifndef WaddleAvWaylandH
#define WaddleAvWaylandH
#include "av_transport.h"
#include "waddle/av_protocol.h"
#include <wayland-client.h>
/** @brief Opaque caller-owned host video client, created/freed on one event thread. */
typedef struct av_wayland_t av_wayland_t;
/** @brief Borrowed synchronous host request callback, no ownership transfer.
 * @param[in] request Nonnull transient validated geometry/close request.
 * @param[in,out] context Optional retained caller context.
 * @return 0 on queued request, -1 on delivery failure.
 * @note Wayland event thread; may not retain request or destroy client recursively.
 */
typedef int (*av_host_request_t)(const av_message_t *request, void *context);
/** @brief Connect to default Wayland display and bind compositor/shm/xdg-shell.
 * @param[out] client Nonnull result; set NULL on failure; owned until free.
 * @param[in] request Nonnull borrowed callback retained until free.
 * @param[in,out] context Optional caller-owned callback context.
 * @return 0 on success, -1 on connection/global/allocation failure.
 * @note Event thread only, all allocation owned by av_wayland_free.
 */
int av_wayland_init(av_wayland_t **client, av_host_request_t request, void *context);
/** @brief Create a captured-window toplevel backed by three shared pixel regions.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @param[in] message Nonnull validated MsgWindowCreate metadata.
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
/** @brief Apply guest lifecycle/geometry/frame to an existing toplevel.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @param[in] message Nonnull validated destroy/geometry/frame message.
 * @return 0 on accepted/drop-busy frame, -1 on stale id/invalid slot/resource failure.
 * @note Event thread only; frames release ownership only on wl_buffer.release.
 */
int av_wayland_message(av_wayland_t *client, const av_message_t *message);
/** @brief Dispatch pending Wayland events after display FD becomes readable.
 * @param[in,out] client Nonnull initialized event-thread-owned client.
 * @return 0 on success, -1 on display disconnect. May invoke request callback.
 * @note Event thread only; blocks unless caller poll indicates readable FD.
 */
int av_wayland_dispatch(av_wayland_t *client);
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
 * two seconds. Unreleased slots remain consumed on compositor error/timeout.
 */
void av_wayland_free(av_wayland_t **client);
#endif
