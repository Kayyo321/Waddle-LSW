#ifndef WaddleAvLayoutH
#define WaddleAvLayoutH
#include "av_transport.h"
#include "waddle/av_protocol.h"
/** @brief Immutable per-video-slot capacity (32 MiB, supports 3840x2160 BGRA). */
#define AvSlotCapacity 33554432u
/** @brief Metadata/control region reserved before page-aligned pixels. */
#define AvPixelOffset 65536u
/** @brief Shared metadata audio ring offset, naturally 64-byte aligned. */
#define AvAudioHeaderOffset 128u
/** @brief Shared PCM payload offset; 4096 stereo frames (16 KiB). */
#define AvPcmOffset 16384u
/** @brief Audio capacity in frames; caller may trim to eight milliseconds. */
#define AvAudioCapacity 4096u
/** @brief Full fixed mapping size, below wl_shm signed-32-bit size ceiling. */
#define AvMappingBytes (UINT64_C(65536) + UINT64_C(48) * AvSlotCapacity)
/** @brief Initialize metadata only in a quiescent caller-owned aligned mapping.
 * @param[in,out] mapping Nonnull 64-byte aligned mapping[length], caller-owned.
 * @param[in] length Accessible bytes, at least AvMappingBytes.
 * @return 0 success, -1 on insufficient length; no writes on validation failure.
 * @note Single initializer before peer connection; never reset active slots/ring.
 * Pixel payload need not be zeroed; producer writes before publication.
 */
int av_layout_init(void *mapping, size_t length);
/** @brief Validate immutable mapping version/size before accessing any slot.
 * @param[in] mapping Nonnull aligned borrowed mapping[length].
 * @param[in] length Accessible bytes.
 * @return 0 valid fixed ABI, -1 bad magic/version/size/length.
 * @note Pure/thread-safe after initialization; no allocation/ownership transfer.
 */
int av_layout_validate(const void *mapping, size_t length);
/** @brief Obtain a bounded borrowed video header for a pool and buffer index.
 * @param[in,out] mapping Nonnull aligned previously validated mapping[length].
 * @param[in] length Accessible bytes, at least AvMappingBytes.
 * @param[in] pool Window pool 0..15. @param[in] index Slot index 0..2.
 * @return Borrowed aligned header, NULL on invalid indices/length; no allocation.
 * @note Mapping remains caller-owned and alive until compositor release.
 */
window_slot_header_t *av_layout_slot(void *mapping, size_t length, uint32_t pool, uint32_t index);
/** @brief Obtain a validated pixel region offset for one window slot.
 * @param[in] pool Window pool 0..15. @param[in] index Slot index 0..2.
 * @return Mapping byte offset, zero for invalid indices.
 * @note Pure/thread-safe; caller validates mapping size before pointer addition.
 */
uint64_t av_layout_pixels(uint32_t pool, uint32_t index);
#endif
