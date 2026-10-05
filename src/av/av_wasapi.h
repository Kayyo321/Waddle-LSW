#ifndef WaddleAvWasapiH
#define WaddleAvWasapiH
#define COBJMACROS
#include "av_transport.h"
#include <audioclient.h>
/** @brief Caller-owned per-process WASAPI stream; single MTA capture thread.
 * @note Zero-initialize; init owns COM references and event; free releases them.
 * Mapping/PCM ownership stays with caller. No process-wide/system loopback.
 */
typedef struct av_wasapi_t {
    IAudioClient *client;         /**< Owned initialized stream. */
    IAudioCaptureClient *capture; /**< Owned capture service. */
    HANDLE ready_event;           /**< Owned sample-ready event. */
    HANDLE mmcss;                 /**< Owned multimedia scheduling registration. */
    uint32_t discontinuities;     /**< Capture-thread diagnostic count. */
    uint64_t captured_frames;     /**< Capture-thread cumulative frame count. */
} av_wasapi_t;
/** @brief Activate a target process tree's S16LE stereo 48kHz loopback stream.
 * @param[out] audio Nonnull zero-initialized caller-owned instance.
 * @param[in] process_id Nonzero target PID, borrowed identifier.
 * @return S_OK or HRESULT (unsupported Windows/audio engine errors propagated).
 * @note MTA capture thread only; COM initialized by caller. Waits up to 10s for
 * async activation, with independently ref-counted callback lifetime on timeout.
 */
HRESULT av_wasapi_init(av_wasapi_t *audio, DWORD process_id);
/** @brief Drain currently available packets into bounded shared PCM storage.
 * @param[in,out] audio Nonnull initialized stream, exclusively capture-thread owned.
 * @param[in,out] ring Nonnull aligned metadata with fixed immutable PCM format.
 * @param[in,out] pcm Nonnull borrowed mapping of pcm_len accessible bytes.
 * @param[in] pcm_len Accessible PCM payload bytes.
 * @return S_OK or HRESULT; WASAPI buffers released even on ring corruption.
 * @note No allocation; producer only; zero-fills silent packets in bounded chunks.
 */
HRESULT av_wasapi_drain(av_wasapi_t *audio, audio_ring_header_t *ring, uint8_t *pcm,
                        size_t pcm_len);
/** @brief Stop capture and release owned resources; NULL all released handles.
 * @param[in,out] audio Nonnull partially initialized/active/empty instance.
 * @note Capture thread only; idempotent; no return or ownership transfer.
 */
void av_wasapi_free(av_wasapi_t *audio);
#endif
