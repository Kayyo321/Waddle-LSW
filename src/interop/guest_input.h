/**
 * @file guest_input.h
 * @brief Guest keyboard executor for the seamless interop path (IMPL_DESC 3.6.2, 3.6.3, 6.3).
 *
 * Scope. The executor consumes host keyboard frames that the codec has already decoded
 * (InputFocus, InputKey, InputRelease, InputLockState, InputRepeatInfo, KeyboardLayout), applies
 * the lease, epoch, serial and lock-sync rules, and performs Win32 keyboard actions through the
 * injected table below. It does no wire parsing. It also provides the release-all paths for
 * revocation, disconnect and agent exit.
 *
 * Out of scope: the LeaseRevoked, LeaseAck and LeaseReady handshake phases, pointer, touch,
 * tablet and text input, real foreground activation, and the transport. guest_input_admit_serial()
 * is the hook the lease component must call for LeaseAck so that the serial stream stays single-owner.
 *
 * Win32 table. guest_input_win32_t holds one function pointer per Win32 call the executor makes.
 * It is passed by pointer at init and never owned. Production code uses guest_input_win32_native()
 * (Windows only). Tests substitute doubles. activate_keyboard_layout is not in the table because
 * the layout path loads with KLF_ACTIVATE and never calls ActivateKeyboardLayout (IMPL_DESC
 * 3.6.3). Parameter types come from guest_win32_compat.h, so the signatures match Windows exactly.
 *
 * Ownership and threading. The caller owns guest_input_t (no heap, no retained pointers except
 * the borrowed table). One owner thread calls every function on one state. guest_input_init()
 * must be the first call on a state; a zeroed state is not valid.
 */
#ifndef WaddleGuestInputH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleGuestInputH

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest_win32_compat.h"
#include "keyboard_layouts.h"
#include "keyboard_lock.h"

/** @brief Highest Linux evdev code the executor accepts (IMPL_DESC 3.6.3). */
#define GuestInputKeyMax 767u
/** @brief Bytes of the held-key bitset: one bit per code 0..767. */
#define GuestInputHeldBytes 96u
/** @brief Capacity of the lock-sync queue of InputKey and InputRelease frames. */
#define GuestInputPendingCapacity 64u
/** @brief Exact KLID length in bytes (eight hexadecimal digits). */
#define GuestInputKlidBytes 8u
/** @brief Largest InputRepeatInfo interval accepted, in milliseconds (codec bound). */
#define GuestInputRepeatIntervalMax 10000u
/** @brief Largest InputRepeatInfo rate accepted, in keys per second (codec bound). */
#define GuestInputRepeatRateMax 1000u
/** @brief Largest lock mask accepted (Caps, Num and Scroll bits). */
#define GuestInputLocksMax 7u
/** @brief Virtual-key code for the Pause key sent by the executor (AV form, IMPL_DESC 3.6.3). */
#define GuestInputVkPause 0x13u
/** @brief Scan code sent with the Pause virtual key (AV form, no scan flag). */
#define GuestInputScanPause 0x45u

/**
 * @brief Result of every executor call. Ok is zero.
 * Terminal statuses latch the state: every later call except release_all and disconnect returns
 * GuestInputTerminal, and the executor has already released held keys on the way out.
 */
typedef enum guest_input_status_t {
    /** Frame accepted and its effect applied, or a valid frame with nothing to inject. */
    GuestInputOk = 0,
    /** The frame was for an older lease epoch. Its serial was consumed; no effect. Not an error. */
    GuestInputStaleEpoch = 1,
    /** The frame is valid but ordered as a no-op: wrong pair, no active pair, or key-up of a key not held. */
    GuestInputIgnored = 2,
    /** The key is suppressed (KEY_POWER, KEY_SLEEP) or unsupported. Nothing injected; not an error. */
    GuestInputNotInjected = 3,
    /** An InputKey or InputRelease is held in the lock-sync queue until InputLockState arrives. */
    GuestInputQueued = 4,
    /** KeyboardLayout equals the last applied KLID. No Win32 call was made. */
    GuestInputLayoutUnchanged = 5,
    /** A required pointer argument was NULL. Nothing changed. */
    GuestInputNullArgument = 6,
    /** A non-protocol argument is out of range (bad KLID text, repeat info out of bounds). Nothing changed. */
    GuestInputInvalidArgument = 7,
    /** No foreground window or no thread for it. The layout was not changed. */
    GuestInputLayoutNoWindow = 8,
    /** LoadKeyboardLayoutW returned NULL. The layout and the recorded KLID are unchanged. */
    GuestInputLayoutLoadFailed = 9,
    /** PostMessageW failed after a successful load. The recorded KLID is unchanged, so a retry is possible. */
    GuestInputLayoutPostFailed = 10,
    /** TERMINAL. Zero or regressed serial, serial wrap, zero or future epoch, malformed field, or a frame that breaks the lock-plan invariant. */
    GuestInputProtocolViolation = 11,
    /** TERMINAL. SendInput failed for a key or lock record. */
    GuestInputInjectionFailed = 12,
    /** TERMINAL. The lock-sync queue is full. */
    GuestInputQueueOverflow = 13,
    /** TERMINAL when raised by a state transition. One or more release records failed; their held bits remain set. */
    GuestInputReleaseFailed = 14,
    /** TERMINAL. The lease epoch is UINT64_MAX and cannot be advanced again. */
    GuestInputEpochExhausted = 15,
    /** The state is terminal. Nothing was done. */
    GuestInputTerminal = 16
} guest_input_status_t;

/**
 * @brief Win32 call table. Every member must be non-NULL. Borrowed; the executor never frees it.
 * @note Calls happen synchronously on the owner thread. No member may re-enter the executor.
 */
typedef struct guest_input_win32_t {
    /** SendInput. @param count in, records in the array. @param inputs in, nonnull, count records. @param size in, sizeof(INPUT). @return records inserted. */
    UINT (*send_input)(UINT count, LPINPUT inputs, int size);
    /** GetKeyState. @param virtual_key in, VK code. @return SHORT state; bit 0 is the toggle state. */
    SHORT (*get_key_state)(int virtual_key);
    /** LoadKeyboardLayoutW. @param klid in, NUL-terminated 8 hex digits. @param flags in. @return HKL, or NULL on failure. */
    HKL (*load_keyboard_layout)(LPCWSTR klid, UINT flags);
    /** PostMessageW. @return nonzero on success. */
    BOOL (*post_message)(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    /** GetForegroundWindow. @return HWND or NULL. */
    HWND (*get_foreground_window)(void);
    /** GetWindowThreadProcessId. @param process_id out, nullable. @return thread id, zero if the window is invalid. */
    DWORD (*get_window_thread_process_id)(HWND window, LPDWORD process_id);
} guest_input_win32_t;

/** @brief Key class returned by guest_input_translate_key(). */
typedef enum guest_input_key_kind_t {
    /** No supported mapping. Never injected. */
    GuestInputKeyUnsupported = 0,
    /** Set-1 scan code in scan_code, E0 flag in extended. */
    GuestInputKeyScanCode = 1,
    /** Windows virtual key in virtual_key. No scan code. */
    GuestInputKeyVirtualKey = 2,
    /** Pause. Injected as virtual_key 0x13 with scan_code 0x45 and no scan flag, matching AV. */
    GuestInputKeyPause = 3,
    /** KEY_POWER and KEY_SLEEP. Never forwarded (IMPL_DESC 6.3). */
    GuestInputKeySuppressed = 4
} guest_input_key_kind_t;

/** @brief Typed mapping of one evdev code. Plain value; unused fields are zero. */
typedef struct guest_input_translation_t {
    /** Key class. */
    guest_input_key_kind_t kind;
    /** Set-1 scan code for ScanCode, or GuestInputScanPause for Pause. */
    uint8_t scan_code;
    /** Nonzero only for an E0-prefixed ScanCode. */
    uint8_t extended;
    /** Virtual key for VirtualKey, or GuestInputVkPause for Pause. */
    uint16_t virtual_key;
} guest_input_translation_t;

/** @brief One queued lock-sync entry: a key transition or a release-all for the active pair. */
typedef struct guest_input_queued_t {
    /** True for a release-all entry; false for a key entry. */
    bool is_release;
    /** Key press state for key entries. */
    bool pressed;
    /** evdev code for key entries; zero for release entries. */
    uint16_t key;
} guest_input_queued_t;

/**
 * @brief Caller-owned executor state. Initialize with guest_input_init() before any other call.
 * Fields are private to the owner thread. No heap. Scalars are by value.
 */
typedef struct guest_input_t {
    /** Borrowed Win32 table. Never NULL after init. */
    const guest_input_win32_t *win32;
    /** Current lease epoch. Zero before the first revocation (which reserves epoch 1). */
    uint64_t epoch;
    /** Highest accepted protocol serial. Zero before the first serial-bearing frame. Never resets. */
    uint32_t serial;
    /** True once a terminal condition latched. */
    bool terminal;
    /** True while a focus pair is requested and accepted for the current epoch. */
    bool pair_active;
    /** Window of the active pair; valid only when pair_active. */
    uint64_t pair_window;
    /** Generation of the active pair; valid only when pair_active. */
    uint64_t pair_generation;
    /** True from a focus enter until its InputLockState has been applied. */
    bool lock_pending;
    /** Entries in queue; valid entries are queue[0] through queue[queue_count - 1]. */
    uint8_t queue_count;
    /** Queued lock-sync entries in arrival order. */
    guest_input_queued_t queue[GuestInputPendingCapacity];
    /** Number of keys currently held (diagnostic and test invariant). */
    uint32_t held_count;
    /** Held-key bitset: bit n of held[n >> 3] set means evdev code n is down. */
    uint8_t held[GuestInputHeldBytes];
    /** True once a KeyboardLayout has been applied. */
    bool layout_known;
    /** KLID of the last applied KeyboardLayout; valid only when layout_known. */
    uint8_t layout_klid[GuestInputKlidBytes];
    /** True once InputRepeatInfo was accepted. */
    bool repeat_known;
    /** Last accepted repeat delay in milliseconds (diagnostic only). */
    uint32_t repeat_interval_ms;
    /** Last accepted repeat rate per second (diagnostic only). */
    uint32_t repeat_rate;
    /** Lock-planner state for accepted InputLockState updates (keyboard_lock.h). */
    waddle_keyboard_lock_guest_t lock_guest;
} guest_input_t;

/**
 * @brief Native Win32 table for Windows builds: SendInput, GetKeyState, LoadKeyboardLayoutW,
 * PostMessageW, GetForegroundWindow and GetWindowThreadProcessId.
 * @return Non-NULL pointer to a static const table. Lifetime is the program. Windows builds only.
 * @note Thread-safe: returns a constant.
 */
const guest_input_win32_t *guest_input_win32_native(void);

/**
 * @brief Initialize an executor state. Epoch 0, serial 0, no pair, no held keys, not terminal.
 * @param state out, non-NULL; fully written on Ok.
 * @param win32 in, non-NULL, every member non-NULL; borrowed for the state's lifetime.
 * @return GuestInputOk, GuestInputNullArgument. Owner thread only.
 */
guest_input_status_t guest_input_init(guest_input_t *state, const guest_input_win32_t *win32);

/**
 * @brief Admit a serial-bearing frame's serial and lease epoch. Single serial authority.
 *
 * Rule order (IMPL_DESC 3.6.2, input-lease-recovery 4): a serial must be nonzero and strictly
 * greater than the highest accepted serial. The serial is recorded even when the frame is stale.
 * A zero lease or a lease greater than the current epoch is terminal. A lease below the current
 * epoch is stale: the serial is consumed and the caller must not act on the frame.
 *
 * @param state inout, non-NULL. @param serial in, protocol serial from the frame. @param lease in, lease epoch from the frame.
 * @return GuestInputOk (current epoch, act on it), GuestInputStaleEpoch (consumed, no effect),
 *         GuestInputNullArgument, GuestInputTerminal, or GuestInputProtocolViolation (terminal).
 * @note Owner thread only. Lease owners call this for LeaseAck before their own handling.
 */
guest_input_status_t guest_input_admit_serial(guest_input_t *state, uint32_t serial, uint64_t lease);

/**
 * @brief Apply an InputFocus frame.
 *
 * pressed = 1 on a different pair: releases all held keys (release-all), discards the lock queue,
 * records the new pair and arms lock sync. The same pair while active is a no-op.
 * pressed = 0 on the active pair: releases all held keys, clears the pair and the lock state.
 * Any other pair with pressed = 0 is a no-op. The executor records the pair only and does not
 * call the foreground table (no activation semantics are invented, IMPL_DESC 3.6.2).
 *
 * @param state inout, non-NULL. @param window in, nonzero window id. @param generation in, nonzero.
 * @param serial in. @param lease in. @param pressed in, 0 or 1.
 * @return GuestInputOk, GuestInputIgnored, GuestInputStaleEpoch, a terminal status, or GuestInputNullArgument.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_focus(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease, uint8_t pressed);

/**
 * @brief Apply an InputKey frame for the active pair in the current epoch.
 *
 * While lock sync is pending the frame is queued in arrival order (GuestInputQueued). Otherwise a
 * key is translated with guest_input_translate_key(): suppressed and unsupported keys are not
 * injected. A down injects a press and marks the key held. A repeated down for a held repeatable
 * key injects a plain down with no intervening up. A repeated down for any other held key is not
 * injected. An up injects a release only for a held key; an up for a key that is not held is ignored.
 *
 * @param state inout, non-NULL. @param window in. @param generation in. @param serial in. @param lease in.
 * @param key in, evdev code 1..767. @param pressed in, 0 or 1.
 * @return GuestInputOk, GuestInputQueued, GuestInputIgnored, GuestInputNotInjected, GuestInputStaleEpoch,
 *         a terminal status, or GuestInputNullArgument.
 * @note Owner thread only. Injection failure is terminal and releases held keys.
 */
guest_input_status_t guest_input_key(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease, uint32_t key, uint8_t pressed);

/**
 * @brief Apply an InputRelease frame: release all held keys of the active pair and stay active.
 * @param state inout, non-NULL. @param window in. @param generation in. @param serial in. @param lease in.
 * @return GuestInputOk, GuestInputQueued (while lock sync is pending), GuestInputIgnored,
 *         GuestInputStaleEpoch, a terminal status, or GuestInputNullArgument.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_release(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease);

/**
 * @brief Apply an InputLockState frame: plan the lock toggles and drain the lock queue.
 *
 * The guest reads GetKeyState for VK_CAPITAL, VK_NUMLOCK and VK_SCROLL, computes the plan with
 * waddle_keyboard_lock_guest_update(), and injects a press and a release for each differing lock.
 * Then the queued entries are applied in arrival order. Without an active pair the frame has no
 * effect (serial consumed).
 *
 * @param state inout, non-NULL. @param serial in. @param locks in, mask 0..7 (bit0 Caps, bit1 Num, bit2 Scroll). @param lease in.
 * @return GuestInputOk, GuestInputIgnored, GuestInputStaleEpoch, a terminal status, or GuestInputNullArgument.
 * @note Owner thread only. A plan that the planner refuses as stale is a terminal protocol violation.
 */
guest_input_status_t guest_input_lock_state(guest_input_t *state, uint32_t serial, uint8_t locks, uint64_t lease);

/**
 * @brief Record InputRepeatInfo for diagnostics. No serial, no lease, no injection.
 * @param state inout, non-NULL. @param interval_ms in, 0..GuestInputRepeatIntervalMax. @param rate in, 0..GuestInputRepeatRateMax.
 * @return GuestInputOk, GuestInputInvalidArgument (state unchanged), GuestInputNullArgument, or GuestInputTerminal.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_repeat_info(guest_input_t *state, uint32_t interval_ms, uint32_t rate);

/**
 * @brief Apply a KeyboardLayout frame: validate, compare, load and notify the foreground thread.
 *
 * Order: validate the KLID; an equal recorded KLID returns GuestInputLayoutUnchanged with no Win32
 * call; otherwise find the foreground window and its thread, then LoadKeyboardLayoutW with
 * KLF_ACTIVATE | KLF_SETFORPROCESS, then PostMessageW(WM_INPUTLANGCHANGEREQUEST) to that window.
 * The recorded KLID changes only after the post succeeds. No serial, no lease.
 *
 * @param state inout, non-NULL. @param klid in, borrowed, length GuestInputKlidBytes, 8 hex digits.
 * @param length in, byte count of klid.
 * @return GuestInputOk, GuestInputLayoutUnchanged, GuestInputInvalidArgument, GuestInputLayoutNoWindow,
 *         GuestInputLayoutLoadFailed, GuestInputLayoutPostFailed, GuestInputNullArgument or GuestInputTerminal.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_keyboard_layout(guest_input_t *state, const uint8_t *klid, size_t length);

/**
 * @brief Guest-side revocation: release all held keys, clear the pair, then reserve the next epoch.
 *
 * Releases happen before the caller queues LeaseRevoked (input-lease-recovery 5). The epoch
 * advances exactly once, and only after the releases succeed. A failed release is terminal and
 * no epoch is reserved. The caller queues LeaseRevoked with *next_epoch.
 *
 * @param state inout, non-NULL. @param next_epoch out, non-NULL; written only on Ok.
 * @return GuestInputOk, GuestInputReleaseFailed (terminal), GuestInputEpochExhausted (terminal),
 *         GuestInputNullArgument, or GuestInputTerminal.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_revoke(guest_input_t *state, uint64_t *next_epoch);

/**
 * @brief Release every held key with a key-up, in ascending evdev order. Allowed in any state.
 *
 * Used for focus leave, window retirement and agent exit (IMPL_DESC 6.3). Keys whose release
 * failed keep their held bit so a later call retries them. Failed releases do not stop the others.
 *
 * @param state inout, non-NULL.
 * @return GuestInputOk, GuestInputReleaseFailed, or GuestInputNullArgument.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_release_all(guest_input_t *state);

/**
 * @brief Disconnect: release all held keys, then latch terminal. Allowed in any state.
 * @param state inout, non-NULL.
 * @return GuestInputOk, GuestInputReleaseFailed, or GuestInputNullArgument. The state is terminal
 *         in every case.
 * @note Owner thread only.
 */
guest_input_status_t guest_input_disconnect(guest_input_t *state);

/**
 * @brief Typed mapping of one evdev code. Equivalent to av_input_translate_key() (proved over 1..767 by the test).
 * @param key in, evdev code. Any value; codes outside 1..767 are unsupported.
 * @param out out, non-NULL; always written on Ok.
 * @return GuestInputOk or GuestInputNullArgument. Pure and thread-safe.
 */
guest_input_status_t guest_input_translate_key(uint32_t key, guest_input_translation_t *out);

/**
 * @brief Whether a duplicate down of this key is an admitted host typematic repeat.
 * Equivalent to av_input_key_repeatable() (proved over 0..768 by the test).
 * @param key in, evdev code. @return 1 for repeatable keys in 1..767, 0 otherwise. Pure and thread-safe.
 */
int guest_input_key_repeatable(uint32_t key);

#endif
