/** @file keyboard_inhibit.h @brief C ABI for the keyboard shortcut-inhibit policy reducer. */
#ifndef WaddleKeyboardInhibitH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeyboardInhibitH
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Policy: request only while the focused window is fullscreen or pointer-locked. Default and zero value. */
#define WaddleKeyboardInhibitFullscreen 0u
/** @brief Policy: never request an inhibitor. */
#define WaddleKeyboardInhibitNever 1u
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
/** @brief Event kind: surface fullscreen and pointer-lock flags of a window generation; accepted for unfocused windows. */
#define WaddleKeyboardInhibitEventSurfaceState 4u
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
/** @brief Bounded number of windows whose flags are remembered. */
#define WaddleKeyboardInhibitMaxTrackedWindows 16u

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
    WaddleKeyboardInhibitInvalidMode = 4,
    /** A flag byte is neither 0 nor 1; state and output unchanged. */
    WaddleKeyboardInhibitInvalidFlag = 5,
    /** A state byte is out of range; state and output unchanged. */
    WaddleKeyboardInhibitInvalidState = 6,
    /** The remembered-window table is full for a new window; state and output unchanged. */
    WaddleKeyboardInhibitWindowTableFull = 7
} waddle_keyboard_inhibit_status_t;

/**
 * @brief One remembered window. Window zero marks a free slot. Private to the module.
 */
typedef struct waddle_keyboard_inhibit_entry_t {
    /** Window identity; zero for a free slot. */
    uint64_t window;
    /** Newest generation seen for this window. */
    uint32_t generation;
    /** 1 when fullscreen, 0 otherwise. */
    uint8_t fullscreen;
    /** 1 when pointer-locked, 0 otherwise. */
    uint8_t pointer_lock;
} waddle_keyboard_inhibit_entry_t;

/**
 * @brief Policy state, 280 bytes. Zero-filled storage is the valid initial value
 * (policy Fullscreen, unfocused, nothing remembered); waddle_keyboard_inhibit_init
 * sets the same. Caller owns storage; one owner thread per state; no allocation.
 * Fields are private: callers must not write them. Every byte is range-checked
 * by the ABI before use, and an out-of-range byte returns InvalidState.
 */
typedef struct waddle_keyboard_inhibit_state_t {
    /** Policy: a WaddleKeyboardInhibit policy value (0 Fullscreen, 1 Never, 2 Focused). */
    uint8_t config;
    /** Inhibitor phase: 0 None, 1 Pending, 2 Active. */
    uint8_t phase;
    /** 1 while a window holds keyboard focus, 0 otherwise. */
    uint8_t focused;
    /** 1 while the user override suppresses inhibition for this focus, 0 otherwise. */
    uint8_t override;
    /** Focused window identity; meaningful only when `focused` is 1. */
    uint64_t window;
    /** Focused window generation; meaningful only when `focused` is 1. */
    uint32_t generation;
    /** Remembered windows. */
    waddle_keyboard_inhibit_entry_t table[WaddleKeyboardInhibitMaxTrackedWindows];
} waddle_keyboard_inhibit_state_t;

/**
 * @brief One event for waddle_keyboard_inhibit_apply. Plain value; no ownership.
 * Both flag bytes must be 0 or 1 for every kind. Fields a kind does not use must be zero.
 */
typedef struct waddle_keyboard_inhibit_event_t {
    /** One of the WaddleKeyboardInhibitEvent values. */
    uint8_t kind;
    /** Policy value, used only by the Configure event. */
    uint8_t mode;
    /** 0 or 1: surface is fullscreen. Used only by the SurfaceState event. */
    uint8_t fullscreen;
    /** 0 or 1: surface is pointer-locked. Used only by the SurfaceState event. */
    uint8_t pointer_lock;
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
 * @brief Set state to the zero value: policy Fullscreen, unfocused, nothing remembered.
 * @param state out, non-NULL.
 * @return Ok or NullArgument. Caller serializes calls per state.
 */
waddle_keyboard_inhibit_status_t waddle_keyboard_inhibit_init(waddle_keyboard_inhibit_state_t *state);

/**
 * @brief Apply one event to the policy state.
 * @param state inout, non-NULL; written only on Ok.
 * @param event in, non-NULL; decoded and validated before state is touched.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, InvalidIdentity, InvalidKind, InvalidMode, InvalidFlag,
 *         InvalidState, or WindowTableFull. Caller serializes calls per state.
 */
waddle_keyboard_inhibit_status_t waddle_keyboard_inhibit_apply(waddle_keyboard_inhibit_state_t *state, const waddle_keyboard_inhibit_event_t *event, waddle_keyboard_inhibit_result_t *out);

#endif
