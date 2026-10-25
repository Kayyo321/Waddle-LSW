/** @file keyboard_inhibit.h @brief C ABI for the keyboard shortcut-inhibit policy reducer. */
#ifndef WaddleKeyboardInhibitH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeyboardInhibitH
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Policy: never request an inhibitor. */
#define WaddleKeyboardInhibitNever 0u
/** @brief Policy: request only while the focused window is fullscreen or pointer-locked. */
#define WaddleKeyboardInhibitFullscreen 1u
/** @brief Policy: request whenever the window holds input focus. */
#define WaddleKeyboardInhibitFocused 2u

/** @brief Event kind: focus enter (window, generation). */
#define WaddleKeyboardInhibitEventFocusEnter 0u
/** @brief Event kind: focus leave (window, generation). */
#define WaddleKeyboardInhibitEventFocusLeave 1u
/** @brief Event kind: compositor reported the inhibitor active. */
#define WaddleKeyboardInhibitEventInhibitorActive 2u
/** @brief Event kind: compositor reported the inhibitor inactive. */
#define WaddleKeyboardInhibitEventInhibitorInactive 3u
/** @brief Event kind: surface fullscreen or pointer-locked changed; uses `fullscreen`. */
#define WaddleKeyboardInhibitEventSurfaceFullscreen 4u
/** @brief Event kind: user escape chord toggled the override. */
#define WaddleKeyboardInhibitEventUserToggle 5u
/** @brief Event kind: window destroyed. */
#define WaddleKeyboardInhibitEventWindowDestroy 6u
/** @brief Event kind: policy configuration changed; uses `mode`. */
#define WaddleKeyboardInhibitEventConfigure 7u

/** @brief Action kind: create one inhibitor object for the identity. */
#define WaddleKeyboardInhibitActionCreate 1u
/** @brief Action kind: destroy the live inhibitor object for the identity. */
#define WaddleKeyboardInhibitActionDestroy 2u
/** @brief Upper bound on actions in one result. */
#define WaddleKeyboardInhibitMaxActions 2u

/** @brief Result code of every ABI call; Ok is zero. */
typedef enum waddle_keyboard_inhibit_status_t {
    /** Operation succeeded; output arguments were written. */
    WaddleKeyboardInhibitOk = 0,
    /** A required pointer argument was NULL; nothing was written. */
    WaddleKeyboardInhibitNullArgument = 1,
    /** Window is zero or generation is zero; state and output unchanged. */
    WaddleKeyboardInhibitInvalidIdentity = 2,
    /** Event kind is not defined; state and output unchanged. */
    WaddleKeyboardInhibitInvalidKind = 3,
    /** Configure mode is not defined; state and output unchanged. */
    WaddleKeyboardInhibitInvalidMode = 4
} waddle_keyboard_inhibit_status_t;

/**
 * @brief Policy state. Caller-owned storage; zero-filled storage is the valid
 * initial value (use waddle_keyboard_inhibit_init). Fields are private: callers
 * must not write them. One owner thread per state; no allocation.
 */
typedef struct waddle_keyboard_inhibit_state_t {
    /** Configured policy; a WaddleKeyboardInhibit mode value. */
    uint8_t config;
    /** Live inhibitor phase: 0 None, 1 Pending, 2 Active. */
    uint8_t phase;
    /** True while a window holds keyboard focus. */
    bool focused;
    /** True while the focused surface is fullscreen or pointer-locked. */
    bool fullscreen;
    /** True while the user override suppresses inhibition for this focus. */
    bool override;
    /** Focused window identity; meaningful only when `focused`. */
    uint64_t window;
    /** Focused window generation; meaningful only when `focused`. */
    uint32_t generation;
} waddle_keyboard_inhibit_state_t;

/**
 * @brief One event for waddle_keyboard_inhibit_apply. Plain value; no ownership.
 * Fields not used by `kind` are ignored.
 */
typedef struct waddle_keyboard_inhibit_event_t {
    /** One of the WaddleKeyboardInhibitEvent values. */
    uint8_t kind;
    /** Policy value, used only by the Configure event. */
    uint8_t mode;
    /** Fullscreen flag, used only by the SurfaceFullscreen event. */
    bool fullscreen;
    /** Window identity. */
    uint64_t window;
    /** Window generation. */
    uint32_t generation;
} waddle_keyboard_inhibit_event_t;

/** @brief One object action: Create or Destroy for an identity. */
typedef struct waddle_keyboard_inhibit_action_t {
    /** One of the WaddleKeyboardInhibitAction values. */
    uint8_t kind;
    /** Window the object belongs to. */
    uint64_t window;
    /** Generation the object belongs to. */
    uint32_t generation;
} waddle_keyboard_inhibit_action_t;

/**
 * @brief Result of one event. Entries at index `count` and beyond are zero.
 * Execute actions in index order.
 */
typedef struct waddle_keyboard_inhibit_result_t {
    /** Number of valid actions; 0 through WaddleKeyboardInhibitMaxActions. */
    uint8_t count;
    /** Actions in execution order. */
    waddle_keyboard_inhibit_action_t actions[WaddleKeyboardInhibitMaxActions];
    /** True when Super and Alt-Tab style shortcuts must be forwarded to the guest. */
    bool forward_shortcuts;
} waddle_keyboard_inhibit_result_t;

/**
 * @brief Set state to the zero value: unfocused, policy Never, no object.
 * @param state out, non-NULL.
 * @return Ok or NullArgument. Thread-safe per state: caller serializes.
 */
waddle_keyboard_inhibit_status_t waddle_keyboard_inhibit_init(waddle_keyboard_inhibit_state_t *state);

/**
 * @brief Apply one event to the policy state.
 * @param state inout, non-NULL; written only on Ok.
 * @param event in, non-NULL; copied and validated before state is touched.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, InvalidIdentity, InvalidKind, or InvalidMode.
 *         Caller serializes calls per state.
 */
waddle_keyboard_inhibit_status_t waddle_keyboard_inhibit_apply(waddle_keyboard_inhibit_state_t *state, const waddle_keyboard_inhibit_event_t *event, waddle_keyboard_inhibit_result_t *out);

#endif
