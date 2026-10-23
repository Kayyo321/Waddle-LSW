/** @file venus_present.h @brief Event-thread zero-copy DMA-BUF presenter. */
#ifndef WaddleVenusPresentH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusPresentH
#include "venus_dmabuf.h"
struct wl_display;
struct wl_surface;
struct zwp_linux_dmabuf_v1;
/** @brief Opaque single-event-thread owner; one allocation freed by free.
 * @note Owns feedback/import/buffer/frame proxies and bounded metadata; borrows
 * display/global/surface/callback through free. Never copy or reenter callbacks.
 */
typedef struct venus_present_t venus_present_t;
/** @brief Notify accepted-frame retirement on the owner's event thread.
 * @param[in,out] context Nullable borrowed caller context retained until free.
 * @param[in] frame Nonzero accepted caller frame identity.
 * @param[in] status RingOk on buffer release; Invalid import rejection; Corrupt
 * proxy failure; Closed display loss. No fd/proxy ownership transfer.
 * @note May not reenter/destroy presenter. Only RingOk permits normal GPU reuse;
 * Closed requires discarding old-session allocation ownership.
 */
typedef void (*venus_present_done_t)(void *context, uint64_t frame, venus_ring_status_t status);
/** @brief Acquire surface feedback and bounded metadata ownership.
 * @param[out] presenter Nonnull initially-NULL owner, unchanged on failure.
 * @param[in] display Nonnull borrowed display, alive until free completes.
 * @param[in] dmabuf Nonnull borrowed version >=4 global from same display/queue.
 * @param[in] surface Nonnull borrowed version >=4 target with role managed by caller.
 * @param[in] done Nonnull borrowed callback, retained until free.
 * @param[in,out] context Nullable borrowed caller context, retained until free.
 * @return RingOk, RingInvalid arguments/version, RingCorrupt allocation/listener failure.
 * @note Event thread only. Owns one metadata allocation, no pixel allocation/map/copy.
 */
venus_ring_status_t venus_present_create(venus_present_t **presenter, struct wl_display *display,
                                         struct zwp_linux_dmabuf_v1 *dmabuf,
                                         struct wl_surface *surface, venus_present_done_t done,
                                         void *context);
/** @brief Queue a validated supported image for asynchronous zero-copy import.
 * @param[in,out] presenter Nonnull event-thread owner with completed feedback.
 * @param[in] frame Monotonically increasing nonzero identity, never reused.
 * @param[in] layout Nonnull immutable private image metadata, copied for import.
 * @param[in] fds Nonnull borrowed array[plane_count] of valid DMA-BUF descriptors,
 * retained only for call. Caller always closes originals after return.
 * @param[in] plane_count Exact accessible fds count matching validated layout.
 * @param[in] damage Nonnull private rectangles[damage_count], copied for import.
 * @param[in] damage_count 1..64; bounds must fit image.
 * @return RingOk accepted (completion callback later); RingAgain feedback/pacing/
 * import/slot backpressure or unsupported pair; Invalid local geometry/FD/ID;
 * Corrupt terminal feedback/proxy failure. Failure transfers no image ownership.
 * @note Sole event thread; retain allocation/no writes until completion. Import
 * rejection permits selecting a new advertised modifier, never CPU copy fallback.
 */
venus_ring_status_t venus_present_submit(venus_present_t *presenter, uint64_t frame,
                                         const venus_dmabuf_layout_t *layout, const int *fds,
                                         size_t plane_count, const venus_dmabuf_damage_t *damage,
                                         size_t damage_count);
/** @brief Release idle owner, or abandon accepted frames after confirmed display error.
 * @param[in,out] presenter Nullable pointer to nullable owner; nulled on success.
 * @return RingOk released/empty; RingAgain healthy display with live import/buffer
 * ownership (retains all storage/proxies). No implicit event loop or deadline wait.
 * @note Event thread only. On wl_display_get_error !=0, destroys local proxies and
 * completes remaining accepted frames Closed; discard old allocations. Free before
 * wl_display_disconnect. Borrowed surface/global/display are never destroyed here.
 */
venus_ring_status_t venus_present_free(venus_present_t **presenter);
#endif
