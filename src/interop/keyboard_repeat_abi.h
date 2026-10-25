/** @file keyboard_repeat_abi.h @brief C ABI for the host key-repeat timing reducer. */
#ifndef WaddleKeyboardRepeatAbiH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeyboardRepeatAbiH
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bytes of caller storage that one reducer occupies. */
#define WaddleKeyboardRepeatStorageBytes 1024u

/** @brief Result code of every ABI call; Ok is zero. */
typedef enum waddle_keyboard_repeat_status_t {
    /** Operation succeeded; output arguments were written. */
    WaddleKeyboardRepeatOk = 0,
    /** A required pointer argument was NULL; nothing was written. */
    WaddleKeyboardRepeatNullArgument = 1,
    /** A flag byte was neither 0 nor 1; state unchanged. */
    WaddleKeyboardRepeatInvalidFlag = 2,
    /** Rate outside 0..1000 or delay outside 0..60000 ms; state unchanged. */
    WaddleKeyboardRepeatInvalidTiming = 3,
    /** Window or generation is zero; state unchanged. */
    WaddleKeyboardRepeatInvalidFocus = 4,
    /** Key code is zero or above 767; state unchanged. */
    WaddleKeyboardRepeatInvalidKey = 5,
    /** The monotonic clock moved backward or a deadline overflowed; repeat was disarmed. */
    WaddleKeyboardRepeatClockDiscontinuity = 6
} waddle_keyboard_repeat_status_t;

/**
 * @brief Caller-owned opaque reducer storage. Zero-filled storage is the valid
 * initial state; waddle_keyboard_repeat_init writes the same state. The bytes are
 * private. Callers must pass the storage only to these functions, from one owner
 * thread. No allocation is performed.
 */
typedef struct waddle_keyboard_repeat_t {
    /** Private reducer bytes; 8-byte aligned. */
    _Alignas(8) unsigned char bytes[WaddleKeyboardRepeatStorageBytes];
} waddle_keyboard_repeat_t;

/**
 * @brief Initialize storage to the disabled, unfocused zero state.
 * @param storage out, non-NULL.
 * @return Ok or NullArgument. Caller serializes calls per storage.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_init(void *storage);

/**
 * @brief Set repeat timing and enable state. A success disarms an active candidate.
 * @param storage inout, non-NULL. @param enabled in, 0 or 1. @param rate in, 0..1000 Hz.
 * @param delay_ms in, 0..60000 ms.
 * @return Ok, NullArgument, InvalidFlag, or InvalidTiming.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_configure(void *storage, uint8_t enabled, int32_t rate, int32_t delay_ms);

/**
 * @brief Replace focus and discard inherited held keys.
 * @param storage inout, non-NULL. @param window in, nonzero. @param generation in, nonzero.
 * @return Ok, NullArgument, or InvalidFocus.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_focus(void *storage, uint64_t window, uint32_t generation);

/**
 * @brief Cancel repeat, clear focus and held observations, keep timing. Idempotent.
 * @param storage inout, non-NULL.
 * @return Ok or NullArgument.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_cancel(void *storage);

/**
 * @brief Observe one physical key event.
 * @param storage inout, non-NULL. @param code in, evdev 1..767.
 * @param pressed in, 0 or 1. @param repeatable in, 0 or 1 from trusted keymap policy.
 * @param now_ns in, monotonic nanoseconds, nondecreasing across calls.
 * @return Ok, NullArgument, InvalidFlag, InvalidKey, or ClockDiscontinuity.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_key(void *storage, uint16_t code, uint8_t pressed, uint8_t repeatable, uint64_t now_ns);

/**
 * @brief Consume at most one due repeat. A returned repeat is never retried by the reducer.
 * @param storage inout, non-NULL. @param now_ns in, monotonic nanoseconds.
 * @param out_code out, non-NULL; receives the evdev code to send as a key down, or 0.
 * @return Ok, NullArgument, or ClockDiscontinuity.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_take_due(void *storage, uint64_t now_ns, uint16_t *out_code);

/**
 * @brief Report whether a repeat is armed and when it is due. Does not mutate state.
 * @param storage in, non-NULL. @param out_present out, non-NULL; 1 when armed, else 0.
 * @param out_deadline out, non-NULL; written with the deadline in ns (0 when not armed).
 * @return Ok or NullArgument.
 */
waddle_keyboard_repeat_status_t waddle_keyboard_repeat_next_deadline(void *storage, uint8_t *out_present, uint64_t *out_deadline);

/**
 * @brief Host repeat policy for one evdev code. Pure and stateless.
 * @param code in; evdev code. @return 1 when the key may auto-repeat, 0 otherwise
 *         (including every code outside 1..767). Must agree with av_input_key_repeatable.
 */
uint8_t waddle_keyboard_repeat_key_repeatable(uint16_t code);

#endif
