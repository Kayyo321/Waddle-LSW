/**
 * @file session_input_handler.h
 * @brief Host-side keyboard feature handler for the session runtime (IMPL_DESC 3.6.2, 3.6.3).
 *
 * The handler implements session_handler_t with capabilities Input and KeyboardLayout. It
 * consumes the guest lease frames (LeaseRevoked, LeaseReady) and CaretRect, and it sends
 * InputFocus, InputKey, InputRelease, InputLockState, InputRepeatInfo, KeyboardLayout and
 * LeaseAck. Input is forwarded only while the lease barrier is complete and a fresh enter
 * arrived after Ready. Wayland objects are not referenced here; the Wayland glue in
 * session_input_wayland.h posts events through the functions below.
 *
 * Ownership: the caller owns every session_input_t and must keep it alive until the
 * session handler's on_closing has run. The handler never allocates. Callback pointers are
 * borrowed for the lifetime of the state.
 *
 * Inhibit: the inhibit reducer's refusals (for example a full 16-window table) are not
 * forwarding failures. Focus and key forwarding continue; only inhibitor actions are missed.
 *
 * Repeat: a backwards clock step forgets the repeat arming. Keys held across the step are
 * not repeated until pressed again.
 *
 * Thread safety: every function, including the post functions, runs on the session pump
 * thread (the thread that calls interop_host_control_run). Not thread-safe.
 */
#ifndef WaddleSessionInputHandlerH
#define WaddleSessionInputHandlerH

#include <stddef.h>
#include <stdint.h>

#include "keyboard_inhibit.h"
#include "keyboard_layouts.h"
#include "keyboard_lock.h"
#include "keyboard_repeat_abi.h"
#include "session_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum queued key, focus, release and lock frames awaiting transmission. */
enum { SessionInputPendingCapacity = 256 };

/** @brief Lease handshake deadline in milliseconds (IMPL_DESC 3.6.2, handshake deadline 2000 ms). */
enum { SessionInputHandshakeMs = 2000 };

/** @brief Highest evdev code the handler forwards (IMPL_DESC 3.6.3, key 1..767). */
enum { SessionInputMaxKey = 767 };

/** @brief Bytes in the held-key bitset (768 codes, one bit each). */
enum { SessionInputHeldBytes = 96 };

/** @brief Result value a callback returns to fail the run (any nonzero value except RequestStop). */
enum { SessionInputCallbackFailed = 2 };

/** @brief Status of a post function. */
typedef enum session_input_post_status_t {
    SessionInputPostOk = 0, /**< Event accepted (it may be queued for a later flush). */
    SessionInputPostInvalidArgument = 1, /**< NULL state or NULL text with nonzero length. */
    SessionInputPostInvalidKey = 2, /**< Evdev code outside 1..767. */
    SessionInputPostKeymapInvalid = 3, /**< Keymap text was rejected; forwarding stopped. */
    SessionInputPostClosed = 4, /**< on_closing has run; the state accepts no further events. */
    SessionInputPostTerminal = 5 /**< The handler failed; the run is ending. */
} session_input_post_status_t;

/** @brief Lease phase of the host side (IMPL_DESC 3.6.2 host barrier). */
typedef enum session_input_phase_t {
    SessionInputAwaitInitialRevocation = 0, /**< No Revoked seen yet; the first must carry epoch 1. */
    SessionInputBarrier = 1, /**< Revoked accepted; LeaseAck not yet sent (retried on tick). */
    SessionInputAwaitReady = 2, /**< LeaseAck sent; waiting for a matching LeaseReady. */
    SessionInputReady = 3, /**< Barrier complete; input may be requested by a fresh enter. */
    SessionInputTerminal = 4 /**< Protocol violation or deadline expiry; the run fails. */
} session_input_phase_t;

/** @brief Kinds of queued frames. Serials are assigned only when a frame is actually sent. */
typedef enum session_input_pending_kind_t {
    SessionInputPendingKey = 0, /**< InputKey with pressed and key. */
    SessionInputPendingFocus = 1, /**< InputFocus with pressed (1 enter, 0 leave). */
    SessionInputPendingRelease = 2, /**< InputRelease for the entry's window and generation. */
    SessionInputPendingLocks = 3 /**< InputLockState with the locks bitmask. */
} session_input_pending_kind_t;

/** @brief One queued frame. Plain data; no pointers. */
typedef struct session_input_pending_t {
    uint8_t kind; /**< session_input_pending_kind_t. */
    uint8_t pressed; /**< Key and Focus entries: 0 or 1. */
    uint16_t code; /**< Key entries: evdev code 1..767. */
    uint8_t locks; /**< Lock entries: bit 0 caps, bit 1 num, bit 2 scroll. */
    uint64_t window; /**< Guest window (HWND) of the pair the frame refers to. */
    uint64_t generation; /**< Guest window incarnation of the pair. */
} session_input_pending_t;

/**
 * @brief Callback that applies keyboard inhibit actions (the Wayland glue creates or destroys
 *        zwp_keyboard_shortcuts_inhibitor_v1 objects). Runs on the pump thread, synchronously
 *        inside the post or handler call that produced the result.
 * @param[in] context Caller context passed at init; may be NULL.
 * @param[in] result Actions and the forward_shortcuts flag; borrowed for the call only.
 */
typedef void (*session_input_inhibit_fn)(void *context, const waddle_keyboard_inhibit_result_t *result);

/**
 * @brief Optional display dispatch hook the handler calls from on_fds when the registered
 *        descriptor reports events (the Wayland glue reads and dispatches its display here).
 * @param[in] context Caller context passed to session_input_set_display; may be NULL.
 * @return Zero on success; nonzero fails the run.
 */
typedef int (*session_input_dispatch_fn)(void *context);

/**
 * @brief Complete handler state. Caller-owned storage; the handler never allocates.
 * @note Fields are private to session_input_handler.c. Initialize only with
 *       session_input_init, and never copy a live state.
 */
typedef struct session_input_t {
    uint8_t inhibit_mode; /**< Keyboard inhibit configuration (WaddleKeyboardInhibit* mode). */
    session_input_inhibit_fn inhibit_fn; /**< Receives inhibit actions; may be NULL. */
    void *inhibit_context; /**< Passed to inhibit_fn unchanged. */
    int display_fd; /**< Wayland display descriptor to multiplex, or -1. */
    session_input_dispatch_fn dispatch; /**< Called from on_fds, or NULL. */
    void *dispatch_context; /**< Passed to dispatch unchanged. */
    uint8_t input_enabled; /**< Input capability negotiated. */
    uint8_t layout_enabled; /**< KeyboardLayout capability negotiated. */
    uint8_t closed; /**< on_closing has run. */
    uint8_t phase; /**< session_input_phase_t. */
    uint64_t epoch; /**< Highest guest lease epoch accepted (0 before the first Revoked). */
    uint32_t serial; /**< Last host protocol serial consumed by a sent frame. */
    uint32_t ack_serial; /**< Serial of the LeaseAck sent in AwaitReady. */
    uint64_t handshake_deadline_ms; /**< Monotonic ms deadline; valid in Barrier and AwaitReady. */
    uint8_t focused; /**< Physical keyboard focus is on a window. */
    uint64_t focus_window; /**< Physical focused window. */
    uint64_t focus_generation; /**< Physical focused window generation. */
    uint8_t forwarding; /**< Input frames are being sent for the pair below. */
    uint64_t pair_window; /**< Requested pair window; valid after the first forwarded enter. */
    uint64_t pair_generation; /**< Requested pair generation. */
    uint8_t release_all_pending; /**< A release-all for the pair must be sent before pending frames. */
    uint8_t terminal; /**< Set on failure; every callback then reports failure. */
    uint16_t pending_count; /**< Entries used in pending. */
    session_input_pending_t pending[SessionInputPendingCapacity]; /**< FIFO of frames not yet sent. */
    uint16_t held_count; /**< Forwarded keys currently down (sent or pending). */
    uint8_t held[SessionInputHeldBytes]; /**< Bitset over evdev codes 0..767. */
    waddle_keyboard_lock_host_t lock_host; /**< Lock sync reducer state. */
    waddle_keyboard_lock_indices_t indices; /**< Caps, Num and Scroll modifier indices. */
    waddle_keyboard_layouts_state_t layouts; /**< Layout reducer state. */
    uint8_t klid[WaddleKeyboardLayoutsKlidBytes]; /**< Last KLID observed from a keymap. */
    uint8_t klid_known; /**< klid holds a value. */
    uint8_t layout_dirty; /**< A KeyboardLayout frame still has to be sent. */
    waddle_keyboard_repeat_t repeat; /**< Repeat timing reducer (1024-byte opaque storage). */
    int32_t repeat_rate; /**< Last wl_keyboard.repeat_info rate per second. */
    int32_t repeat_delay_ms; /**< Last wl_keyboard.repeat_info delay in milliseconds. */
    uint8_t repeat_known; /**< repeat_rate and repeat_delay_ms hold a value. */
    uint8_t repeat_dirty; /**< An InputRepeatInfo frame still has to be sent. */
    uint64_t repeats_dropped; /**< Repeat frames dropped because the queue was full or blocked. */
    uint64_t overflows; /**< Pending overflows resolved by release-all. */
    waddle_keyboard_inhibit_state_t inhibit; /**< Shortcut inhibit reducer state. */
    interop_host_feature_sender_t *sender; /**< Valid only inside a callback; NULL otherwise. */
} session_input_t;

/**
 * @brief Initialize caller-owned handler state.
 * @param[out] input Non-NULL storage written completely.
 * @param[in] inhibit_mode WaddleKeyboardInhibitFullscreen, Never or Focused.
 * @param[in] inhibit_fn Optional action receiver; may be NULL (no inhibitors then).
 * @param[in] inhibit_context Passed to inhibit_fn unchanged; may be NULL.
 * @return SessionInputPostOk, or SessionInputPostInvalidArgument for a NULL input or
 *         an invalid inhibit mode.
 * @note No allocation. Must be called before the state is registered.
 */
int session_input_init(session_input_t *input, uint8_t inhibit_mode,
    session_input_inhibit_fn inhibit_fn, void *inhibit_context);

/**
 * @brief Build the session_handler_t that routes runtime callbacks to the state.
 * @param[in] input Initialized state that outlives the run.
 * @return A handler with name "session_input", capabilities Input|KeyboardLayout and the
 *         owns_type set LeaseRevoked, LeaseReady and CaretRect. Returned by value; no allocation.
 */
session_handler_t session_input_make_handler(session_input_t *input);

/**
 * @brief Register the Wayland display descriptor and the dispatch hook for on_fds.
 * @param[in,out] input Initialized state.
 * @param[in] display_fd Descriptor (nonblocking, not closed by the handler), or -1 to clear.
 * @param[in] dispatch Hook called from on_fds when the descriptor reports events; may be NULL
 *            only with display_fd -1.
 * @param[in] dispatch_context Passed to dispatch unchanged; may be NULL.
 * @return SessionInputPostOk or SessionInputPostInvalidArgument.
 */
int session_input_set_display(session_input_t *input, int display_fd,
    session_input_dispatch_fn dispatch, void *dispatch_context);

/**
 * @brief Report that a window received keyboard focus (wl_keyboard.enter).
 * @param[in,out] input Initialized state.
 * @param[in] window Guest window identifier (HWND value) of the surface.
 * @param[in] generation Guest window incarnation from the host window map; never 0.
 * @return SessionInputPostOk, Closed or Terminal. A previously focused pair is left first.
 * @note Forwarding starts only when the lease phase is Ready. An enter received in any other
 *       phase is recorded as physical focus only; it is never replayed after Ready.
 */
int session_input_post_focus_enter(session_input_t *input, uint64_t window, uint64_t generation);

/**
 * @brief Report that the keyboard focus left the window (wl_keyboard.leave).
 * @param[in,out] input Initialized state.
 * @return SessionInputPostOk, Closed or Terminal. Held keys are released before the focus-off.
 */
int session_input_post_focus_leave(session_input_t *input);

/**
 * @brief Report one physical key transition (wl_keyboard.key).
 * @param[in,out] input Initialized state.
 * @param[in] code Evdev code (XKB keycode - 8), 1..SessionInputMaxKey.
 * @param[in] pressed 0 for release, nonzero for press.
 * @return SessionInputPostOk, InvalidKey, Closed or Terminal. Keys are forwarded only while
 *         forwarding; otherwise the call has no effect on the guest.
 */
int session_input_post_key(session_input_t *input, uint32_t code, uint8_t pressed);

/**
 * @brief Report the modifier masks (wl_keyboard.modifiers).
 * @param[in,out] input Initialized state.
 * @param[in] depressed Depressed modifier mask.
 * @param[in] latched Latched modifier mask.
 * @param[in] locked Locked modifier mask; bits per the keymap indices.
 * @return SessionInputPostOk, Closed or Terminal.
 */
int session_input_post_modifiers(session_input_t *input, uint32_t depressed, uint32_t latched, uint32_t locked);

/**
 * @brief Report a keymap (wl_keyboard.keymap, format xkb_v1), already read into memory.
 * @param[in,out] input Initialized state.
 * @param[in] text Keymap bytes, read only during the call; may be NULL only with length 0.
 * @param[in] length Byte count, at most WaddleKeymapMaxBytes.
 * @return SessionInputPostOk on a valid keymap; SessionInputPostKeymapInvalid when the
 *         keymap is rejected, which also queues a focus-off for the requested pair and stops
 *         forwarding; SessionInputPostClosed or Terminal.
 */
int session_input_post_keymap(session_input_t *input, const uint8_t *text, size_t length);

/**
 * @brief Report wl_keyboard.repeat_info.
 * @param[in,out] input Initialized state.
 * @param[in] rate Repeats per second; zero disables repeat.
 * @param[in] delay_ms Delay before the first repeat in milliseconds.
 * @return SessionInputPostOk, Closed or Terminal.
 */
int session_input_post_repeat_info(session_input_t *input, int32_t rate, int32_t delay_ms);

/**
 * @brief Report an inhibit input event (surface fullscreen or pointer lock, inhibitor state,
 *        user toggle, configure or destroy). Focus enter and leave are reported by the focus
 *        functions above and must not be passed here.
 * @param[in,out] input Initialized state.
 * @param[in] event Event fields; borrowed for the call.
 * @return SessionInputPostOk, InvalidArgument, Closed or Terminal. Actions reach inhibit_fn.
 */
int session_input_post_inhibit(session_input_t *input, const waddle_keyboard_inhibit_event_t *event);

#ifdef __cplusplus
}
#endif
#endif
