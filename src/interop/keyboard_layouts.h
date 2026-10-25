/** @file keyboard_layouts.h @brief C ABI for XKB layout to Windows KLID sync and send-on-change reduction. */
#ifndef WaddleKeyboardLayoutsH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeyboardLayoutsH
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum bytes of one XKB layout or variant name. */
#define WaddleKeyboardLayoutsMaxNameBytes 64u
/** @brief Exact length of a KLID string (eight hexadecimal digits). */
#define WaddleKeyboardLayoutsKlidBytes 8u
/** @brief Outcome kind: the mapped KLID differs from the last sent one; send one KeyboardLayout. */
#define WaddleKeyboardLayoutsSend 0u
/** @brief Outcome kind: the mapped KLID equals the last sent one; send nothing. */
#define WaddleKeyboardLayoutsUnchanged 1u
/** @brief Outcome kind: no table row matches; the guest layout stays unchanged. */
#define WaddleKeyboardLayoutsNoMapping 2u

/** @brief Result code of every ABI call; Ok is zero. */
typedef enum waddle_keyboard_layouts_status_t {
    /** Operation succeeded; output arguments were written. */
    WaddleKeyboardLayoutsOk = 0,
    /** A required pointer argument was NULL; nothing was written. */
    WaddleKeyboardLayoutsNullArgument = 1,
    /** A name is empty, longer than 64 bytes, or outside the XKB charset; nothing was written. */
    WaddleKeyboardLayoutsInvalidName = 2,
    /** KLID text is not eight non-zero hexadecimal digits; nothing was written. */
    WaddleKeyboardLayoutsInvalidKlid = 3
} waddle_keyboard_layouts_status_t;

/** @brief Windows KLID in canonical uppercase ASCII hexadecimal. Plain value. */
typedef struct waddle_keyboard_layouts_klid_t {
    /** Digits 0 through 9 and A through F. */
    uint8_t bytes[WaddleKeyboardLayoutsKlidBytes];
} waddle_keyboard_layouts_klid_t;

/**
 * @brief Host state for the last KeyboardLayout sent on one connection.
 * Zero-initialized value is valid (nothing sent). One host thread per state.
 */
typedef struct waddle_keyboard_layouts_state_t {
    /** True once a KeyboardLayout has been sent on this connection. */
    bool has_applied;
    /** KLID of the last sent KeyboardLayout; valid only when `has_applied`. */
    waddle_keyboard_layouts_klid_t applied;
} waddle_keyboard_layouts_state_t;

/** @brief Result of one observation. `klid` is zero when `kind` is NoMapping. */
typedef struct waddle_keyboard_layouts_outcome_t {
    /** One of WaddleKeyboardLayoutsSend, WaddleKeyboardLayoutsUnchanged, WaddleKeyboardLayoutsNoMapping. */
    uint8_t kind;
    /** Mapped KLID, or zero for NoMapping. */
    waddle_keyboard_layouts_klid_t klid;
} waddle_keyboard_layouts_outcome_t;

/**
 * @brief Validate KLID text and write its canonical uppercase form.
 * @param text in; borrowed for the call; required when len is nonzero.
 * @param len in; byte count of text.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidKlid. Thread-safe: pure.
 */
waddle_keyboard_layouts_status_t waddle_keyboard_layouts_validate_klid(const uint8_t *text, size_t len, waddle_keyboard_layouts_klid_t *out);

/**
 * @brief Observe the host layout after a keymap change or on connect.
 * @param state inout, non-NULL; written only on Ok.
 * @param layout in; borrowed for the call; required when layout_len is nonzero.
 * @param layout_len in; 1 through 64 for a valid name.
 * @param variant in; may be NULL when variant_len is 0 (default variant).
 * @param variant_len in; 0 through 64.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidName. Caller serializes calls per state.
 */
waddle_keyboard_layouts_status_t waddle_keyboard_layouts_observe(waddle_keyboard_layouts_state_t *state, const uint8_t *layout, size_t layout_len, const uint8_t *variant, size_t variant_len, waddle_keyboard_layouts_outcome_t *out);

/**
 * @brief Forget the last sent KLID, for example on disconnect.
 * @param state inout, non-NULL.
 * @return Ok or NullArgument. Caller serializes calls per state.
 */
waddle_keyboard_layouts_status_t waddle_keyboard_layouts_reset(waddle_keyboard_layouts_state_t *state);

#endif
