#ifndef WaddleAvPipewireH
#define WaddleAvPipewireH
#include "av_transport.h"
#include <pipewire/pipewire.h>
/** @brief Caller-owned host playback stream, one active instance per process.
 * @note Ring and PCM are borrowed and retained from init through free. init owns
 * stream/thread-loop; realtime callback exclusively owns ring read_head. Control
 * thread must not concurrently free. No allocation occurs in process callback.
 */
typedef struct av_pipewire_t {
    struct pw_thread_loop *loop;      /**< Owned PipeWire worker loop. */
    struct pw_stream *stream;         /**< Owned fixed-format playback stream. */
    audio_ring_header_t *ring;        /**< Borrowed aligned ring metadata. */
    const uint8_t *pcm;               /**< Borrowed shared PCM payload. */
    size_t pcm_len;                   /**< Accessible shared PCM payload bytes. */
    _Atomic uint32_t underrun_frames; /**< Callback-owned, control acquire-load only. */
    _Atomic uint32_t invalid_buffers; /**< Callback-owned error count. */
    _Atomic int state; /**< Callback-published stream state, control acquire-load only. */
    int library_initialized;          /**< Owns matching pw_init/pw_deinit lifecycle. */
} av_pipewire_t;
/** @brief Start fixed 48kHz stereo S16LE PipeWire playback.
 * @param[out] audio Nonnull zero-initialized caller-owned stream.
 * @param[in,out] ring Nonnull aligned initialized SPSC metadata, borrowed until free.
 * @param[in] pcm Nonnull payload retained until free, no concurrent producer overwrite.
 * @param[in] pcm_len Accessible payload bytes, at least ring capacity * 4.
 * @return 0 on connected paused/streaming state, -1 on validation/allocation/connection failure.
 * @note Control thread only; readiness waits at most five seconds. Connection alone
 * does not prove audible samples; session checks asynchronous failures.
 */
int av_pipewire_init(av_pipewire_t *audio, audio_ring_header_t *ring, const uint8_t *pcm,
                     size_t pcm_len);
/** @brief Test whether a connected stream remains healthy.
 * @param[in] audio Nonnull initialized borrowed instance.
 * @return 1 paused/streaming, 0 disconnected/error; no allocation or ownership change.
 * @note Control thread; atomic acquire-load synchronized with state callback.
 */
int av_pipewire_ready(const av_pipewire_t *audio);
/** @brief Stop callback thread before freeing stream and borrowed mapping references.
 * @param[in,out] audio Nonnull partially initialized/active/empty instance.
 * @note Control thread only, idempotent; caller may unmap PCM after return.
 */
void av_pipewire_free(av_pipewire_t *audio);
#endif
