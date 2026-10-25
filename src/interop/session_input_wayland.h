/**
 * @file session_input_wayland.h
 * @brief Wayland glue between wl_keyboard, the shortcut-inhibit protocol and the host keyboard
 *        handler (session_input_handler.h; IMPL_DESC 3.6.2, 3.6.3).
 *
 * The glue is the only host keyboard component that references Wayland objects. It turns
 * wl_keyboard events into session_input_post_* calls, maps wl_surface objects to guest windows,
 * and applies the handler's inhibit actions to zwp_keyboard_shortcuts_inhibitor_v1 objects.
 *
 * Scope: keymap delivery (xkb_v1 only), enter, leave, key, modifiers and repeat_info, a fixed
 * surface-to-window table, the shortcut inhibitor lifecycle with its active and inactive events,
 * and surface fullscreen and pointer-lock state for the inhibit policy.
 * Out of scope: producing surface state (the caller tracks fullscreen and pointer lock), seat
 * capability tracking, creating or releasing wl_keyboard and wl_surface objects, display
 * connection and dispatch (the caller registers the display through session_input_set_display),
 * and layout or keymap parsing beyond the handler's own checks.
 *
 * Ownership:
 * - The glue owns no Wayland object. The caller owns the display, the seat, the shortcut-inhibit
 *   manager, every wl_surface and every wl_keyboard. The caller calls session_input_detach_keyboard
 *   before releasing a wl_keyboard, and calls session_input_unmap_surface before destroying a
 *   wl_surface.
 * - The glue owns the single zwp_keyboard_shortcuts_inhibitor_v1 it creates. It destroys that
 *   object in the inhibit action that releases it, and in session_input_wayland_destroy.
 * - session_input_wayland_t is caller-owned storage. It must not move or be freed while a keyboard
 *   listener, an inhibitor listener or the handler's inhibit callback can still run.
 * - The glue performs no heap allocation.
 *
 * Thread safety: not thread-safe. Every function in this header and every Wayland listener callback
 * runs on the pump thread that dispatches the Wayland display.
 */
#ifndef WaddleSessionInputWaylandH
#define WaddleSessionInputWaylandH

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyboard_inhibit.h"
#include "session_input_handler.h"

/* Opaque libwayland and generated protocol types; this header only names them by pointer. */
struct wl_keyboard;
struct wl_seat;
struct wl_surface;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Capacity of the surface-to-window table. A full table rejects new mappings. */
enum { MaxSurfaceEntries = 64 };

/** @brief Result of a glue function. Ok is zero. */
typedef enum session_input_wayland_status_t {
    SessionInputWaylandOk = 0, /**< Operation completed. */
    SessionInputWaylandInvalidArgument = 1, /**< A NULL pointer, zero identity, out-of-range generation, or invalid mode. Nothing changed. */
    SessionInputWaylandTableFull = 2, /**< The surface table has MaxSurfaceEntries entries; the mapping was not added. */
    SessionInputWaylandAlreadyAttached = 3, /**< A keyboard is already attached; nothing changed. */
    SessionInputWaylandNotAttached = 4, /**< session_input_detach_keyboard without an attached keyboard. */
    SessionInputWaylandListenerFailed = 5, /**< wl_proxy_add_listener refused the keyboard listener; nothing changed. */
    SessionInputWaylandHandlerRejected = 6 /**< The handler returned a non-Ok post status; see session_input_last_post_status. */
} session_input_wayland_status_t;

/**
 * @brief One surface-to-window mapping. A free slot has surface NULL.
 */
typedef struct session_input_wayland_entry_t {
    struct wl_surface *surface; /**< Borrowed wl_surface; NULL marks a free slot. Caller owns the surface. */
    uint64_t window; /**< Guest window identity (HWND value); nonzero when in use. */
    uint64_t generation; /**< Guest window incarnation; nonzero and at most UINT32_MAX when in use. */
} session_input_wayland_entry_t;

/**
 * @brief Complete glue state. Caller-owned storage; fields are private to session_input_wayland.c.
 *        Zero-filled storage is never valid: call session_input_wayland_init first.
 */
typedef struct session_input_wayland_t {
    session_input_t *input; /**< Handler state; borrowed and must outlive the glue. */
    struct wl_keyboard *keyboard; /**< Attached keyboard, borrowed; NULL when detached. */
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager; /**< Borrowed; NULL disables inhibitors. */
    struct wl_seat *seat; /**< Borrowed; the seat passed to inhibit_shortcuts when a manager is set. */
    struct wl_surface *focused_surface; /**< Surface of the last accepted enter; NULL when the glue has no focus. */
    struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor; /**< Live inhibitor object owned by the glue; NULL when none. */
    uint64_t inhibitor_window; /**< Window identity of the live inhibitor. */
    uint64_t inhibitor_generation; /**< Generation of the live inhibitor. */
    int last_post_status; /**< session_input_post_status_t of the most recent handler call made by the glue. */
    session_input_wayland_entry_t entries[MaxSurfaceEntries]; /**< Fixed surface table. */
} session_input_wayland_t;

/**
 * @brief Initialize the glue and the handler state it drives.
 *
 * Calls session_input_init(input, inhibit_mode, session_input_wayland_apply_inhibit, glue), so the
 * caller must not call session_input_init again for this input. Then clears the glue and stores
 * the borrowed pointers. The inhibit mode is one of WaddleKeyboardInhibitFullscreen, Never or
 * Focused.
 *
 * @param[out] glue Non-NULL caller storage, written completely.
 * @param[in,out] input Non-NULL handler state; written by session_input_init.
 * @param[in] inhibit_mode Handler inhibit policy.
 * @param[in] manager_or_null Shortcut-inhibit manager, borrowed; NULL means no inhibitors are created.
 * @param[in] seat_or_null Seat used with inhibit_shortcuts; required (non-NULL) when manager is non-NULL.
 * @return SessionInputWaylandOk; SessionInputWaylandInvalidArgument for a NULL glue, a NULL input,
 *         a manager without a seat, or an inhibit mode the handler rejects.
 * @note Thread safety: pump thread only.
 */
int session_input_wayland_init(session_input_wayland_t *glue, session_input_t *input, uint8_t inhibit_mode,
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager_or_null, struct wl_seat *seat_or_null);

/**
 * @brief Destroy the live inhibitor object, if any, and clear the glue to the zero state.
 *
 * Does not post a focus leave: the caller stops the handler (or unmaps and detaches) before calling.
 * Does not release the wl_keyboard; the caller releases it after session_input_detach_keyboard.
 *
 * @param[in,out] glue Non-NULL glue previously initialized; zero-filled on return.
 * @note Thread safety: pump thread only.
 */
void session_input_wayland_destroy(session_input_wayland_t *glue);

/**
 * @brief Register the keyboard listener on a wl_keyboard.
 *
 * Installs listener callbacks that run on the pump thread. The glue stores the keyboard pointer
 * and passes glue as listener data; the caller keeps the keyboard alive until it calls
 * session_input_detach_keyboard.
 *
 * @param[in,out] glue Non-NULL initialized glue.
 * @param[in] keyboard Non-NULL wl_keyboard, borrowed.
 * @return SessionInputWaylandOk; InvalidArgument for NULL arguments; AlreadyAttached when a keyboard
 *         is attached; ListenerFailed when wl_keyboard_add_listener returns nonzero (glue unchanged).
 */
int session_input_attach_keyboard(session_input_wayland_t *glue, struct wl_keyboard *keyboard);

/**
 * @brief Forget the attached keyboard. Delivers a focus leave when the glue has focus.
 *
 * The caller releases the wl_keyboard after this call returns. The listener stays registered
 * until the proxy is destroyed, and the caller must not dispatch it after release.
 *
 * @param[in,out] glue Non-NULL initialized glue.
 * @return SessionInputWaylandOk, NotAttached, or HandlerRejected when the focus leave failed.
 */
int session_input_detach_keyboard(session_input_wayland_t *glue);

/**
 * @brief Add or update the mapping from a wl_surface to a guest window.
 *
 * A surface already in the table has its identity replaced. When that surface holds keyboard
 * focus and the identity changed, the glue reports a focus enter for the new identity; the handler
 * leaves the old pair itself, which releases its inhibitor.
 *
 * @param[in,out] glue Non-NULL initialized glue.
 * @param[in] surface Non-NULL wl_surface, borrowed until unmapped.
 * @param[in] window Guest window identity; nonzero.
 * @param[in] generation Guest window incarnation; nonzero and at most UINT32_MAX.
 * @return SessionInputWaylandOk; InvalidArgument for NULL arguments, a zero window or generation, or
 *         a generation above UINT32_MAX; TableFull when no free slot exists and the surface is new;
 *         HandlerRejected when the re-enter after a focused identity change failed.
 */
int session_input_map_surface(session_input_wayland_t *glue, struct wl_surface *surface, uint64_t window, uint64_t generation);

/**
 * @brief Remove a surface from the table.
 *
 * When the surface holds focus, the glue first reports a focus leave, which releases held keys and
 * destroys the inhibitor. Unmapping a surface that is not in the table succeeds and changes nothing.
 *
 * @param[in,out] glue Non-NULL initialized glue.
 * @param[in] surface Non-NULL wl_surface.
 * @return SessionInputWaylandOk (also for an unknown surface); InvalidArgument for NULL arguments;
 *         HandlerRejected when the focus leave failed (the entry is removed in every case).
 */
int session_input_unmap_surface(session_input_wayland_t *glue, struct wl_surface *surface);

/**
 * @brief Report fullscreen and pointer-lock state for a guest window generation.
 *
 * The caller is the window-tracking component, which is outside the keyboard tranche. The glue does
 * not read the surface table here; the handler applies the state to its policy.
 *
 * @param[in,out] glue Non-NULL initialized glue.
 * @param[in] window Guest window identity.
 * @param[in] generation Guest window incarnation; at most UINT32_MAX.
 * @param[in] fullscreen True when the surface is fullscreen.
 * @param[in] pointer_lock True when the surface holds a pointer lock.
 * @return SessionInputWaylandOk; InvalidArgument for a NULL glue or a generation above UINT32_MAX;
 *         HandlerRejected for an identity the handler refuses (for example window zero).
 */
int session_input_post_surface_state(session_input_wayland_t *glue, uint64_t window, uint64_t generation,
    bool fullscreen, bool pointer_lock);

/**
 * @brief Inhibit action receiver; the session_input_inhibit_fn installed by session_input_wayland_init.
 *
 * Applies each action in index order. Create: when a manager and a mapped surface for the identity
 * exist, creates an inhibitor, listens to its active and inactive events and stores it; a live
 * inhibitor is destroyed first. Destroy: destroys the live inhibitor when its identity matches.
 * Missing manager, unknown identity, a failed create, or a failed listener registration skip the
 * action without an error. Unknown action kinds are ignored.
 *
 * @param[in] context The session_input_wayland_t passed at init; must be non-NULL.
 * @param[in] result Borrowed for the call; count is at most WaddleKeyboardInhibitMaxActions.
 * @note Runs synchronously inside the handler call that produced the result, on the pump thread.
 */
void session_input_wayland_apply_inhibit(void *context, const waddle_keyboard_inhibit_result_t *result);

/**
 * @brief Return the handler status of the most recent handler call made by the glue.
 * @param[in] glue Non-NULL initialized glue.
 * @return A session_input_post_status_t value; SessionInputPostOk after init. Wayland callbacks cannot
 *         return status, so the caller reads terminal and keymap-rejection outcomes here.
 * @note Thread safety: pump thread only.
 */
int session_input_last_post_status(const session_input_wayland_t *glue);

#ifdef __cplusplus
}
#endif
#endif
