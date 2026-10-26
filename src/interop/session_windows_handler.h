#ifndef WaddleSessionWindowsHandlerH
#define WaddleSessionWindowsHandlerH
/**
 * @file session_windows_handler.h
 * @brief Windows feature handler of the waddle session: the Wayland layer as a runtime path (TODO #11g live wiring,
 * #12 host half of move, resize and state requests, #13 host half of the wire window path).
 *
 * The handler implements @c session_handler_t. It owns one Wayland connection (host_wayland_core.h) and the window
 * realization (host_wayland_windows.h), and one window-state reducer per realized window (window_state.h, keyed by
 * window pair). It consumes the window messages of 3.2.5 and answers the guest with the frames of 4.3, 4.5 and 4.7.
 *
 * LIMITS, read before relying on this handler (the 11 credit covers window lifecycle only):
 *  - FRAMES ARE NOT BUILT. FrameReady is answered at once with FrameRelease for the exact slot and frame sequence, so
 *    the guest never leaks a slot. Nothing is attached or committed. The counter `frames_dropped_no_buffer_layer`
 *    counts each one. This is the limit of the frames layer, not of the window lifecycle.
 *  - Error frames are never sent by this handler. Error is a control-class message (control_peer.zig), and a feature
 *    sender refuses control types. A refused guest request or a refused window is counted (counters.errors_reported,
 *    refusals) and dropped; the run continues.
 *  - WindowIconChanged is ignored and counted (counters.icons_ignored). The catalog lane owns icons.
 *  - Persistent maximize and fullscreen are host state planned by the reducer. Minimize is one-shot.
 *  - Popups (menu, tooltip, IME preedit) have no maximize, minimize or fullscreen state, so their host actions are
 *    satisfied without a compositor request.
 *  - The reducer ABI takes a 32-bit generation. A window whose generation has zero low 32 bits (2^32, for example)
 *    gets no reducer: its announce is refused and counted (counters.announces_refused), and the window is not realized.
 *    Other generations above 2^32 work, because the reducer keys by the low 32 bits and the handler keys by all 64.
 *
 * Threading: every function and every callback runs on the pump thread that owns the control runtime. The handler
 * never retains a sender past the callback that received it.
 *
 * Failure model (session_handler.h): a callback returns zero to continue. It returns nonzero to fail the run when the
 * handler cannot continue: a Wayland display error, a missing required global (xdg_wm_base, wl_compositor, wl_shm), a
 * send that is neither success, QueueFull nor Draining, a pending-output overflow (counters.pending_overflow), or an
 * unsolicited frame that carries a nonzero request identifier.
 */
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host_wayland_core.h"
#include "host_wayland_windows.h"
#include "session_control_host.h"
#include "session_handler.h"
#include "window_state.h"

struct wl_display;

/** @brief Pending outbound frames kept while the feature queue is full (3.2 backpressure). */
#define SessionWindowsMaxPending 128u
/** @brief Largest outbound frame the handler encodes. */
#define SessionWindowsFrameBytes 256u
/** @brief Window-state reducers the handler holds, one per realized window (the windows table holds 64). */
#define SessionWindowsMaxReducers 64u
/** @brief Left mouse button, as the evdev code the input glue records in a press (linux/input-event-codes.h). */
#define SessionWindowsButtonLeft 0x110u

/** @brief Startup configuration. Strings and pointers are borrowed for the handler's life. */
/** @brief Callback return value for a hard failure; distinct from SessionHandlerRequestStop (1), which would end the run as a graceful local stop. */
enum { SessionWindowsHandlerFailure = 2 };

typedef struct session_windows_config_t {
    const char *device; /**< Device name, 1..64 bytes, used in app ids. Required. */
    const char *display_name; /**< Display socket name, or NULL for WAYLAND_DISPLAY. */
    struct wl_display *display; /**< Injected display for tests; borrowed and never disconnected. NULL to connect. */
    uint32_t scale_numerator; /**< Host scale fraction numerator; zero with a zero denominator means 1/1. */
    uint32_t scale_denominator; /**< Host scale fraction denominator. */
    uint64_t (*now_ms)(void); /**< Monotonic clock in ms; NULL uses CLOCK_MONOTONIC. */
} session_windows_config_t;

/** @brief Counters, readable at any time. Every field only increases between two calls of init. */
typedef struct session_windows_counters_t {
    uint64_t frames_received; /**< Admitted inbound frames this handler owns. */
    uint64_t frames_sent; /**< Outbound frames accepted by the feature sender. */
    uint64_t frames_queued; /**< Outbound frames stored in the pending list. */
    uint64_t frames_dropped_draining; /**< Outbound frames refused with Draining (closing). */
    uint64_t frames_dropped_no_buffer_layer; /**< FrameReady frames answered with FrameRelease (see the limits). */
    uint64_t frames_dropped_retired; /**< Pending window frames dropped because their window was retired before they were sent. */
    uint64_t configures_coalesced; /**< Superseded WindowConfigure frames replaced in the pending list. */
    uint64_t pending_overflow; /**< Outbound frames that could not be stored; each one failed the run. */
    uint64_t announces_realized; /**< WindowAnnounce frames that realized a window. */
    uint64_t announces_refused; /**< WindowAnnounce frames refused or unrealizable (on_error reported by the windows layer). */
    uint64_t updates_applied; /**< WindowUpdate frames applied. */
    uint64_t retires_applied; /**< WindowRetire frames applied. */
    uint64_t moves_applied; /**< WindowMoveRequest frames that started an interactive move. */
    uint64_t moves_ignored; /**< WindowMoveRequest frames rejected by the validation of 4.5. */
    uint64_t resizes_applied; /**< WindowResizeRequest frames that started an interactive resize. */
    uint64_t resizes_ignored; /**< WindowResizeRequest frames rejected by the validation of 4.5. */
    uint64_t state_requests_applied; /**< WindowStateRequest frames that changed the host maximize state. */
    uint64_t state_requests_ignored; /**< WindowStateRequest frames refused by the reducer or the windows layer. */
    uint64_t guest_observations_applied; /**< Guest state observations completed by the reducer. */
    uint64_t host_actions_failed; /**< Host compositor requests of a plan that failed; the reducer resynchronizes. */
    uint64_t configures_sent; /**< WindowConfigure frames queued or sent. */
    uint64_t closes_sent; /**< WindowClose frames queued or sent. */
    uint64_t popup_frames_sent; /**< PopupRepositioned and PopupDismissed frames queued or sent. */
    uint64_t icons_ignored; /**< WindowIconChanged frames ignored (catalog lane). */
    uint64_t errors_reported; /**< Refusals reported by the windows layer, counted instead of sent (see the limits). */
    uint64_t frame_fields_refused; /**< Inbound frames whose required fields could not be read. */
} session_windows_counters_t;

/**
 * @brief Handler state. Private fields. The caller owns the storage; it must not move while the handler runs.
 *
 * Reducer and pending storage are fixed arrays. The window-state reducer allocates through the caller's allocator
 * (window_state_abi.zig), which is the only allocation the handler makes.
 */
typedef struct session_windows_handler_t {
    session_windows_config_t config; /**< Copied configuration. */
    uint32_t scale; /**< Integer host scale, 1..8, resolved at init. */
    uint8_t initialized; /**< Nonzero after init. */
    uint8_t wayland_ready; /**< Nonzero after a successful on_ready. */
    uint8_t closing; /**< Nonzero from on_closing. */
    uint8_t failed; /**< Nonzero after a fatal condition; every later callback returns nonzero. */
    uint64_t lease_epoch; /**< Input lease that presses must belong to for a guest request to act. */
    host_wayland_core_t core; /**< The Wayland connection. */
    host_wayland_t windows; /**< The window realization. */
    host_wayland_core_seat_listener_t seat_listener; /**< Input lane seat hook, forwarded after the handler's own forget. */
    void *seat_user; /**< Passed unchanged to seat_listener. */
    struct {
        uint64_t window;
        uint64_t generation;
        waddle_window_state_handle_t *handle;
        uint8_t used;
        uint8_t popup; /**< Nonzero for a popup role (menu, tooltip, IME): its host actions need no compositor request. */
    } reducers[SessionWindowsMaxReducers]; /**< One reducer per realized window, found by window pair. */
    struct {
        uint8_t bytes[SessionWindowsFrameBytes];
        size_t length;
        uint32_t type;
        uint64_t window;
        uint64_t generation;
    } pending[SessionWindowsMaxPending]; /**< Outbound frames waiting for the feature queue, in order. */
    uint32_t pending_count; /**< Live entries of pending. */
    interop_host_feature_sender_t *sender; /**< The sender of the running callback, NULL outside callbacks. */
    session_windows_counters_t counters; /**< Counters. */
} session_windows_handler_t;

/**
 * @brief Initialize a handler. Does not connect to the compositor; that happens in on_ready.
 *
 * @param handler Out. Caller-owned storage, overwritten.
 * @param config In. Non-NULL. device must be 1..64 bytes; the scale must resolve to 1..8.
 * @return 0 on success; nonzero for a NULL pointer, a bad device name, or a bad scale fraction.
 */
int session_windows_handler_init(session_windows_handler_t *handler, const session_windows_config_t *config);

/**
 * @brief The callback table to register with the session runtime. Valid while the handler lives.
 *
 * @param handler In/out. Initialized handler; its address is the table's context.
 * @return The table. The owns_type, on_frame and poll_fds members are never NULL.
 */
session_handler_t session_windows_handler_descriptor(session_windows_handler_t *handler);

/**
 * @brief Copy the counters.
 *
 * @param handler In. Initialized handler; NULL gives zeros.
 * @param out In. Non-NULL storage; written with the counters.
 */
void session_windows_handler_counters(const session_windows_handler_t *handler, session_windows_counters_t *out);

/**
 * @brief Set the input lease that presses and guest requests must belong to. Presses of other leases are dropped.
 *
 * @param handler In/out. Initialized handler.
 * @param lease_epoch In. The current lease epoch.
 */
void session_windows_set_lease_epoch(session_windows_handler_t *handler, uint64_t lease_epoch);

/**
 * @brief Record a physical press for a later guest request (3.6.2). The input glue calls this on every press.
 *
 * @param handler In/out. Initialized handler; before on_ready the press cannot be recorded (NotInitialized).
 * @param input In. The press, as host_wayland_press_input_t.
 * @return The status of host_wayland_windows_record_press.
 */
host_wayland_status_t session_windows_record_press(session_windows_handler_t *handler, const host_wayland_press_input_t *input);

/**
 * @brief Remove the press for protocol serial T (release or cancel).
 *
 * @param handler In/out. Initialized handler; NULL is a no-op.
 * @param protocol_serial In. Host protocol serial T.
 */
void session_windows_release_press(session_windows_handler_t *handler, uint32_t protocol_serial);

/**
 * @brief Register a seat listener that also receives the handler's seat removals (the input lane hook).
 *
 * A listener set on a ready handler is first told every seat the core records, as added, in slot order. Afterwards it
 * receives each seat the core adds or removes; a removed seat is forgotten by the windows layer before the listener sees
 * it. An added seat stays valid until its removal; a removed seat is valid during the call and destroyed right after it
 * returns (host_wayland_core.h).
 *
 * @param handler In/out. Initialized handler.
 * @param listener In. May be NULL to remove it.
 * @param user In. Passed unchanged.
 */
void session_windows_set_seat_listener(session_windows_handler_t *handler, host_wayland_core_seat_listener_t listener, void *user);

#endif
