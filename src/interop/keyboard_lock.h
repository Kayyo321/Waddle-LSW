/** @file keyboard_lock.h @brief C ABI for host lock-state reduction and guest VK toggle planning. */
#ifndef WaddleKeyboardLockH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleKeyboardLockH
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Caps Lock bit of a lock mask (protocol tag Locks bit 0). */
#define WaddleKeyboardLockCaps 0x01u
/** @brief Num Lock bit of a lock mask (bit 1). */
#define WaddleKeyboardLockNum 0x02u
/** @brief Scroll Lock bit of a lock mask (bit 2). */
#define WaddleKeyboardLockScroll 0x04u
/** @brief Union of admitted lock bits; any other bit makes a mask invalid. */
#define WaddleKeyboardLockAll 0x07u
/** @brief Highest keymap modifier index accepted (indices are 0 through 31). */
#define WaddleKeyboardLockMaxModifierIndex 31u
/** @brief Index sentinel meaning the keymap defines no such lock modifier. */
#define WaddleKeyboardLockModifierAbsent 0xFFu
/** @brief Upper bound on toggle actions in one plan. */
#define WaddleKeyboardLockMaxToggleActions 6u

/** @brief Result code of every ABI call; Ok is zero. */
typedef enum waddle_keyboard_lock_status_t {
    /** Operation succeeded; output arguments were written. */
    WaddleKeyboardLockOk = 0,
    /** A required pointer argument was NULL; nothing was written. */
    WaddleKeyboardLockNullArgument = 1,
    /** A modifier index is neither ModifierAbsent nor 0 through 31; nothing was written. */
    WaddleKeyboardLockInvalidIndex = 2,
    /** A lock mask has a bit outside WaddleKeyboardLockAll; nothing was written. */
    WaddleKeyboardLockInvalidMask = 3,
    /** The protocol serial is zero; nothing was written. */
    WaddleKeyboardLockInvalidSerial = 4,
    /** The lease epoch is zero; nothing was written. */
    WaddleKeyboardLockInvalidEpoch = 5
} waddle_keyboard_lock_status_t;

/**
 * @brief Copied wl_keyboard.modifiers masks. Plain value; no ownership.
 * Only `locked` selects lock bits.
 */
typedef struct waddle_keyboard_lock_modifiers_t {
    /** Physically held modifier bits; diagnostic only. */
    uint32_t depressed;
    /** One-shot latched modifier bits; diagnostic only. */
    uint32_t latched;
    /** Locked modifier bits; bit n means XKB modifier index n is locked. */
    uint32_t locked;
} waddle_keyboard_lock_modifiers_t;

/**
 * @brief Keymap indices of the three lock modifiers. Each is ModifierAbsent or 0 through 31.
 */
typedef struct waddle_keyboard_lock_indices_t {
    /** XKB index of Caps Lock, or WaddleKeyboardLockModifierAbsent. */
    uint8_t caps;
    /** XKB index of Num Lock, or WaddleKeyboardLockModifierAbsent. */
    uint8_t num;
    /** XKB index of Scroll Lock, or WaddleKeyboardLockModifierAbsent. */
    uint8_t scroll;
} waddle_keyboard_lock_indices_t;

/**
 * @brief Host owner state. Zero-initialized value is valid and unfocused.
 * Caller owns storage; one serialized host thread per state; no allocation.
 */
typedef struct waddle_keyboard_lock_host_t {
    /** True between a focus enter and the next leave. */
    bool focused;
    /** Lock mask last sent for the current focus. */
    uint8_t last_mask;
} waddle_keyboard_lock_host_t;

/**
 * @brief Host decision for one observation. Plain value; no ownership.
 */
typedef struct waddle_keyboard_lock_host_decision_t {
    /** True when exactly one InputLockState must be queued with `mask`. */
    bool send;
    /** Lock mask computed from the observation. */
    uint8_t mask;
} waddle_keyboard_lock_host_decision_t;

/**
 * @brief Guest owner state for accepted InputLockState updates.
 * Zero-initialized value is valid (nothing accepted). One guest owner thread per state.
 */
typedef struct waddle_keyboard_lock_guest_t {
    /** True once the first update has been accepted. */
    bool accepted;
    /** Lease epoch of the last accepted update; valid only when `accepted`. */
    uint64_t epoch;
    /** Protocol serial of the last accepted update; valid only when `accepted`. */
    uint32_t serial;
} waddle_keyboard_lock_guest_t;

/** @brief One guest VK toggle action; press and release are separate entries. */
typedef struct waddle_keyboard_lock_action_t {
    /** Windows virtual-key code: 0x14 Caps, 0x90 Num, 0x91 Scroll. */
    uint16_t vk;
    /** True for press, false for release. */
    bool pressed;
} waddle_keyboard_lock_action_t;

/** @brief Ordered toggle plan. Valid entries are actions[0] through actions[count - 1]. */
typedef struct waddle_keyboard_lock_plan_t {
    /** Number of valid actions; 0, 2, 4 or 6. */
    uint8_t count;
    /** Actions in injection order. */
    waddle_keyboard_lock_action_t actions[WaddleKeyboardLockMaxToggleActions];
} waddle_keyboard_lock_plan_t;

/** @brief Guest result of one InputLockState. */
typedef struct waddle_keyboard_lock_update_t {
    /** False when the update was stale; the plan is then empty. */
    bool applied;
    /** Toggle actions to inject before forwarding other keys. */
    waddle_keyboard_lock_plan_t plan;
} waddle_keyboard_lock_update_t;

/**
 * @brief Compute the lock mask from modifiers and keymap indices.
 * @param mods in, non-NULL; copied. @param indices in, non-NULL; copied.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidIndex. Thread-safe: pure.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_mask(const waddle_keyboard_lock_modifiers_t *mods, const waddle_keyboard_lock_indices_t *indices, uint8_t *out);

/**
 * @brief Host focus enter; always requests one InputLockState.
 * @param state inout, non-NULL; written only on Ok. @param mods in, non-NULL. @param indices in, non-NULL.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidIndex. Caller serializes calls per state.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_host_enter(waddle_keyboard_lock_host_t *state, const waddle_keyboard_lock_modifiers_t *mods, const waddle_keyboard_lock_indices_t *indices, waddle_keyboard_lock_host_decision_t *out);

/**
 * @brief Host modifier observation; sends only on change while focused.
 * @param state inout, non-NULL; written only on Ok. @param mods in, non-NULL. @param indices in, non-NULL.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidIndex. Caller serializes calls per state.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_host_observe(waddle_keyboard_lock_host_t *state, const waddle_keyboard_lock_modifiers_t *mods, const waddle_keyboard_lock_indices_t *indices, waddle_keyboard_lock_host_decision_t *out);

/**
 * @brief Host focus leave; clears focus.
 * @param state inout, non-NULL.
 * @return Ok or NullArgument. Caller serializes calls per state.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_host_leave(waddle_keyboard_lock_host_t *state);

/**
 * @brief Guest InputLockState handling with the epoch and serial staleness gate.
 * @param state inout, non-NULL; written only on Ok.
 * @param epoch in; must be nonzero. @param serial in; must be nonzero.
 * @param desired in; lock mask from the host. @param current in; GetKeyState toggle bits.
 * @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, InvalidEpoch, InvalidSerial, or InvalidMask. Caller serializes per state.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_guest_update(waddle_keyboard_lock_guest_t *state, uint64_t epoch, uint32_t serial, uint8_t desired, uint8_t current, waddle_keyboard_lock_update_t *out);

/**
 * @brief Build the toggle plan turning `current` into `desired`, without guest state.
 * @param desired in; @param current in. @param out out, non-NULL; written only on Ok.
 * @return Ok, NullArgument, or InvalidMask. Thread-safe: pure.
 */
waddle_keyboard_lock_status_t waddle_keyboard_lock_plan_toggles(uint8_t desired, uint8_t current, waddle_keyboard_lock_plan_t *out);

#endif
