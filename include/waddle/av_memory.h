#ifndef WaddleAvMemoryH
#define WaddleAvMemoryH
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Video slot ownership states. No allocation; identical on both peers. */
typedef enum video_slot_state_t {
    /** No reader or writer owns the pixels. */
    SlotFree = 0,
    /** Guest exclusively owns the header and pixels. */
    SlotWriting = 1,
    /** Guest published pixels; host may claim them. */
    SlotReady = 2,
    /** Host/compositor owns pixels until buffer release. */
    SlotConsuming = 3
} video_slot_state_t;

/** @brief One cache-line video header; caller-owned mapped memory, no pointers.
 * @note Guest modifies metadata only in Writing; host reads only in Consuming.
 * Atomic state transfers ownership with acquire/release. Mapping must outlive
 * every slot and compositor buffer. Initialization requires quiescent peers.
 */
typedef struct window_slot_header_t {
    _Alignas(64) _Atomic uint32_t slot_state; /**< Ownership; video_slot_state_t. */
    uint32_t buffer_index; /**< Immutable pool index, 0..2. */
    uint64_t frame_sequence; /**< Nonzero monotonically increasing capture sequence. */
    uint64_t timestamp_ns; /**< Guest monotonic capture time, nanoseconds. */
    uint32_t width; /**< Nonzero pixel width. */
    uint32_t height; /**< Nonzero pixel height. */
    uint32_t stride; /**< Bytes per row, at least width * 4. */
    uint32_t format; /**< BGRA bytes, DRM ARGB8888 with little-endian pixels. */
    uint8_t reserved[24]; /**< Zero padding; stable 64-byte ABI. */
} window_slot_header_t;

/** @brief SPSC stereo S16LE ring metadata, caller-owned 64-byte aligned mapping.
 * @note One guest producer writes write_head and overrun_frames; one host audio
 * callback writes read_head. Format/capacity stay immutable while running.
 * Cursor difference counts frames, wraps modulo 2^32 and never exceeds capacity.
 * Mapping and PCM payload outlive both peers. No allocation or cross-VM locks.
 */
typedef struct audio_ring_header_t {
    _Alignas(64) _Atomic uint32_t write_head; /**< Release-published producer cursor. */
    _Atomic uint32_t read_head; /**< Release-published consumer cursor. */
    uint32_t sample_rate; /**< Always 48000 Hz. */
    uint32_t channels; /**< Always two interleaved channels. */
    uint32_t format; /**< Always 1: signed 16-bit little-endian. */
    uint32_t capacity_frames; /**< Power of two, 2..2^20 PCM frames. */
    _Atomic uint32_t overrun_frames; /**< Producer-only cumulative dropped frames. */
    uint8_t reserved[36]; /**< Zero padding to 64 bytes. */
} audio_ring_header_t;

/** @brief PCM frame size in bytes; immutable, no ownership/thread restrictions. */
#define AvAudioFrameBytes 4u
/** @brief Fixed PCM sample rate in Hz; immutable. */
#define AvSampleRate 48000u
/** @brief Little-endian DRM ARGB8888; bytes B,G,R,A. */
#define AvPixelFormat 0x34325241u
/** @brief Maximum bounded PCM ring capacity in frames. */
#define AvMaxAudioFrames 1048576u

_Static_assert(sizeof(window_slot_header_t) == 64, "video header ABI");
_Static_assert(_Alignof(window_slot_header_t) == 64, "video alignment ABI");
_Static_assert(offsetof(window_slot_header_t, reserved) == 40, "video field ABI");
_Static_assert(sizeof(audio_ring_header_t) == 64, "audio header ABI");
_Static_assert(_Alignof(audio_ring_header_t) == 64, "audio alignment ABI");
_Static_assert(offsetof(audio_ring_header_t, reserved) == 28, "audio field ABI");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "cross-VM atomics must be lock-free");
#endif
