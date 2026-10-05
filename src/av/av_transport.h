#ifndef WaddleAvTransportH
#define WaddleAvTransportH
#include "waddle/av_memory.h"

/** @brief Claim a free slot for writing without waiting.
 * @param[in,out] slot Nonnull caller-owned aligned header retained until release.
 * @return 1 if claimed, 0 if busy. No allocations; one producer per slot.
 */
int av_video_begin_write(window_slot_header_t *slot);
/** @brief Publish completed pixels after filling slot metadata.
 * @param[in,out] slot Nonnull header exclusively held in SlotWriting.
 * @return 1 on publication, 0 on incorrect ownership. Release orders pixel writes.
 * @note No allocation; producer only; mapping remains owned by caller.
 */
int av_video_publish(window_slot_header_t *slot);
/** @brief Acquire published pixels for host/compositor use.
 * @param[in,out] slot Nonnull aligned borrowed header.
 * @return 1 if claimed, 0 if not Ready. No allocation; one consumer per slot.
 */
int av_video_begin_read(window_slot_header_t *slot);
/** @brief Release host-owned pixels after compositor buffer release.
 * @param[in,out] slot Nonnull borrowed header in SlotConsuming.
 * @return 1 on release, 0 on wrong state. No allocation; consumer only.
 */
int av_video_release(window_slot_header_t *slot);
/** @brief Cancel unpublished capture or discard an unsent ready notification.
 * @param[in,out] slot Nonnull header exclusively owned by producer.
 * @return 1 if Writing/Ready becomes Free, 0 otherwise. Never steals Consuming.
 * @note No allocation; only producer before notification/consumer acquisition.
 */
int av_video_cancel(window_slot_header_t *slot);

/** @brief Copy PCM into a bounded SPSC ring, dropping incoming overflow.
 * @param[in,out] ring Nonnull aligned borrowed immutable-format ring metadata.
 * @param[in,out] pcm Nonnull borrowed payload with pcm_len bytes.
 * @param[in] pcm_len Accessible payload bytes, at least capacity_frames * 4.
 * @param[in] input Nonnull borrowed PCM bytes; input_len divisible by four.
 * @param[in] input_len Accessible input bytes; input and pcm must not overlap.
 * @return Number of frames copied, or -1 for corrupt format/cursors/lengths.
 * @note No allocation; exactly one producer. Failure leaves cursors unchanged.
 */
int64_t av_audio_write(audio_ring_header_t *ring, uint8_t *pcm, size_t pcm_len,
                       const uint8_t *input, size_t input_len);
/** @brief Consume PCM into a bounded playback buffer, filling underrun silence.
 * @param[in,out] ring Nonnull aligned borrowed immutable-format ring metadata.
 * @param[in] pcm Nonnull borrowed ring bytes retained throughout this call.
 * @param[in] pcm_len Accessible payload bytes, at least capacity_frames * 4.
 * @param[out] output Nonnull caller-owned output; output_len divisible by four.
 * @param[in] output_len Accessible output bytes; output and pcm must not overlap.
 * @param[in] max_backlog Maximum retained frames, 0 disables trimming.
 * @return Frames copied (excluding silence), -1 for invalid metadata/lengths.
 * @note No allocation; exactly one consumer; errors leave output/cursors unchanged.
 */
int64_t av_audio_read(audio_ring_header_t *ring, const uint8_t *pcm, size_t pcm_len,
                      uint8_t *output, size_t output_len, uint32_t max_backlog);
/** @brief Validate a BGRA frame against a bounded pixel region.
 * @param[in] width Nonzero width in pixels.
 * @param[in] height Nonzero height in pixels.
 * @param[in] stride Bytes per row, at least width * 4.
 * @param[in] capacity Accessible pixel bytes.
 * @return Required bytes, or zero for invalid/overflowing dimensions.
 * @note Pure, thread-safe, allocation-free; no ownership transfer.
 */
uint64_t av_video_size(uint32_t width, uint32_t height, uint32_t stride, uint64_t capacity);
/** @brief Check center 3x3 BGRA pixels against a diagnostic RGB24 token.
 * @param[in] pixels Nonnull borrowed pixels[length], held under slot read ownership.
 * @param[in] length Accessible bytes. @param[in] width Pixel width, 3..8192.
 * @param[in] height Pixel height, 3..8192. @param[in] stride Row byte stride.
 * @param[in] token Nonzero RGB24 token.
 * @return 1 exact match, 0 stale or invalid layout. Pure/thread-safe, no allocation.
 */
int av_target_matches(const uint8_t *pixels, size_t length, uint32_t width,
                      uint32_t height, uint32_t stride, uint32_t token);
#endif
