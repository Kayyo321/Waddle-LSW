/** @file venus_surface.h @brief Credential-bound worker frame to Wayland surface ownership. */
#ifndef WaddleVenusSurfaceH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusSurfaceH
#include "venus_frame.h"
#include "venus_present.h"
/** @brief Opaque event-thread owner; one allocation freed by free.
 * @note Owns presenter, pending received FDs and three accepted frame records.
 * Borrows socket/unreaped worker identity/display/global/surface/callback until free.
 */
typedef struct venus_surface_t venus_surface_t;
/** @brief Report frame completion to the controller's release-acknowledgement owner.
 * @param[in,out] context Nullable borrowed callback context retained until free.
 * @param[in] frame Nonnull immutable metadata, borrowed only for callback duration.
 * @param[in] status RingOk buffer release, Invalid rejected/local unsupported import,
 * Corrupt proxy failure, Closed display loss, Cancelled pending frame abandoned.
 * @note Event thread only; no reentry/destruction. Copy identities if acknowledgement
 * needs queuing. Only release permits normal reuse; loss requires old-context discard.
 */
typedef void (*venus_surface_done_t)(void *context, const venus_frame_t *frame,
                                     venus_ring_status_t status);
/** @brief Bind one retained worker context to an existing native window surface.
 * @param[out] owner Nonnull initially-NULL private pointer, unchanged on failure.
 * @param[in] socket_fd Borrowed prepared nonblocking CLOEXEC seqpacket endpoint.
 * @param[in] worker_pid Positive live/unreaped PID; caller prevents identity reuse.
 * @param[in] identity Nonzero exact controller context identity.
 * @param[in] display Borrowed live display, retained through free.
 * @param[in] dmabuf Borrowed version-four global on the same display/queue.
 * @param[in] surface Borrowed version-four role-bound surface, retained through free.
 * @param[in] done Nonnull callback; caller owns release acknowledgement delivery.
 * @param[in,out] context Nullable borrowed callback state, retained through free.
 * @return RingOk, Invalid arguments/socket, Corrupt allocation/protocol acquisition.
 * @note Event thread only. No socket/worker/window ownership transfer or pixel copy.
 */
venus_ring_status_t venus_surface_create(venus_surface_t **owner, int socket_fd, int32_t worker_pid,
                                         uint64_t identity, struct wl_display *display,
                                         struct zwp_linux_dmabuf_v1 *dmabuf,
                                         struct wl_surface *surface, venus_surface_done_t done,
                                         void *context);
/** @brief Bind a surface and own bounded native release acknowledgement retries.
 * @param[out] owner Nonnull initially-NULL private pointer, unchanged on failure.
 * @param[in] socket_fd Borrowed prepared nonblocking CLOEXEC seqpacket endpoint.
 * @param[in] worker_pid Positive retained unreaped worker identity.
 * @param[in] identity Nonzero exact controller context.
 * @param[in] display Borrowed live display retained through free.
 * @param[in] dmabuf Borrowed version-four global on same display/queue.
 * @param[in] surface Borrowed role-bound version-four target surface.
 * @param[in] observer Nullable borrowed completion observer, no reentry/destruction.
 * @param[in,out] context Nullable borrowed observer state retained until free.
 * @return Same acquisition status as venus_surface_create.
 * @note Event thread only. Owns four private acknowledgement slots inside owner;
 * poll/free retry Again before new receipt/teardown. Observer callback does not
 * mean acknowledgement delivery. Caller retains receiver/worker/window lifetimes.
 */
venus_ring_status_t venus_surface_create_acknowledged(venus_surface_t **owner, int socket_fd,
                                                      int32_t worker_pid, uint64_t identity,
                                                      struct wl_display *display,
                                                      struct zwp_linux_dmabuf_v1 *dmabuf,
                                                      struct wl_surface *surface,
                                                      venus_surface_done_t observer, void *context);
/** @brief Receive/retry at most one frame without blocking the Wayland event loop.
 * @param[in,out] owner Nonnull event-thread owner; independently dispatch its display.
 * @return RingOk accepted into presenter; Invalid local import rejection reported or NULL; Again
 * no packet/pacing/feedback/slot backpressure, Corrupt stale/wrong-context packet,
 * Closed peer loss. Native terminal receive/submit results are sticky.
 * @note On Again after receipt, keeps original FDs/metadata for exact retry. On
 * acceptance/failure closes originals; Wayland owns its duplicates on acceptance.
 * In explicit callback mode caller acknowledges completion. Acknowledged mode
 * flushes queued releases first and retains them on Again without receiving.
 * Caller retains GPU allocation until authenticated completion consumption.
 */
venus_ring_status_t venus_surface_poll(venus_surface_t *owner);
/** @brief Abandon one unsubmitted pending frame, allowing modifier/allocation retry.
 * @param[in,out] owner Nonnull event-thread owner.
 * @return RingOk after callback Invalid and descriptor close; Again if none; Invalid NULL.
 * @note Never cancels an accepted compositor-owned buffer. Caller chooses when to
 * abandon unsupported feedback/pacing waits; no implicit timeout or CPU fallback.
 */
venus_ring_status_t venus_surface_cancel_pending(venus_surface_t *owner);
/** @brief Release idle owner or local proxies after confirmed display loss.
 * @param[in,out] owner Nullable pointer to nullable owned state; nulled on success.
 * @return RingOk empty/freed; Again accepted compositor frames still live.
 * @note Event thread only, frees before display disconnect. On successful teardown
 * pending unsubmitted frame completes Cancelled and closes FDs. Acknowledged
 * mode retains owner on send Again even after presenter cleanup. Native terminal
 * send allows idle cleanup with pointer NULL and terminal return; discard old
 * context allocations. Busy presenter still retains listener cookies. Borrowed native
 * socket, window and worker remain caller-owned; stop worker before unbinding.
 */
venus_ring_status_t venus_surface_free(venus_surface_t **owner);
#endif
