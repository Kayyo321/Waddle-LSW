#ifndef WaddleHostWaylandWindowsH
#define WaddleHostWaylandWindowsH
/**
 * @file host_wayland_windows.h
 * @brief Host realization of interop windows as xdg-shell objects (TODO #11b, #11d, #11e, #11f).
 *
 * The module turns WindowAnnounce, WindowUpdate and WindowRetire frames (decoded by the codec in
 * `interop_protocol.h`) into wl_surface, xdg_surface, xdg_toplevel or xdg_popup, and optional
 * zxdg_toplevel_decoration_v1 objects. The window table is `host_window_map.h`, the popup geometry is
 * `host_popup_positioner.h`. Compositor events come back as the callbacks in
 * `host_wayland_callbacks_t`.
 *
 * Ownership and threading:
 *  - The caller owns the `host_wayland_t` storage. It must not move while initialized, and it must
 *    be reset before its storage is released or reused. Its fields are private.
 *  - The Wayland objects in the config and the decoration manager are borrowed. The wl_seat values of the press
 *    table are borrowed too, and must stay valid until `host_wayland_windows_forget_seat` names them or the
 *    context is reset. A press keeps its seat registered until then.
 *  - Single-thread only: the Wayland dispatch thread that owns the display calls every function and every
 *    callback. Callbacks run synchronously, inside the call that caused them, on that same thread.
 *  - The module never allocates. Every object it creates is destroyed by the module on retire, on a failed
 *    realization, or on reset. Destruction order is fixed (see the contract).
 *
 * Return codes and refusals: functions return `host_wayland_status_t`. A refused Announce, Update or Retire
 * also reports `on_error` with the protocol error code. A refused Update changes nothing and sends no request
 * (all-or-nothing, 4.3), and a refused Announce leaves the table as the table rules specify.
 * The full contract is `impl/seamless-interop/claude_45_percent/host_wayland_windows_contract.md`.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host_popup_positioner.h"
#include "host_window_map.h"

struct wl_compositor;
struct wl_seat;
struct xdg_wm_base;
struct zxdg_decoration_manager_v1;

/** @brief Result of every function of this module. */
typedef enum host_wayland_status_t {
    HostWaylandOk = 0, /**< Frame applied, or a stale pair was a no-op. */
    HostWaylandInvalidArgument = 1, /**< NULL context, frame or config pointer. Nothing changed. */
    HostWaylandMalformedFrame = 2, /**< Header or payload failed codec validation or a field is unreadable. Nothing changed. */
    HostWaylandUnsupportedType = 3, /**< A message this module does not realize. Nothing changed. */
    HostWaylandRefused = 4, /**< The window table or the positioner refused the frame; `on_error` reported it. Nothing changed. */
    HostWaylandRealizeFailed = 5, /**< A Wayland object or listener could not be created; the window was rolled back and retired; `on_error` reported it. */
    HostWaylandPressTableFull = 6, /**< All 64 press entries are live; the caller must fail the session (3.6.2). Nothing changed. */
    HostWaylandNotInitialized = 7, /**< The context was not initialized or was reset. */
    HostWaylandDuplicateSerial = 8, /**< A live press already holds this protocol serial T. Nothing changed. */
    HostWaylandSeatTableFull = 9, /**< All `HostWaylandMaxSeats` seats are registered; the press was not recorded. */
    HostWaylandPressNotFound = 10 /**< No live press holds the serial (never recorded, released, expired, or retired). */
} host_wayland_status_t;

/** @brief Seats the press table can name at once. A press on a seat beyond this is refused. */
#define HostWaylandMaxSeats 8u

/** @brief Input class of a press (3.6.2). A popup grab may follow any of them. */
typedef enum host_wayland_press_kind_t {
    HostWaylandPressPointer = 1, /**< Pointer button. `button` is the evdev code and must be nonzero. */
    HostWaylandPressTouch = 2, /**< Touch down. `button` is ignored. */
    HostWaylandPressKey = 3 /**< Keyboard key. `button` is the evdev code. */
} host_wayland_press_kind_t;

/** @brief Everything one press records. Passed by pointer; the module copies it. */
typedef struct host_wayland_press_input_t {
    uint32_t protocol_serial; /**< Host protocol input serial T, nonzero and unique among live entries. */
    uint32_t wayland_serial; /**< Wayland seat serial S, used only at a compositor request. */
    struct wl_seat *seat; /**< Borrowed seat the press came from; registered by the press and forgotten by `forget_seat`. */
    uint64_t window; /**< Window the press landed on. */
    uint64_t generation; /**< Generation of that window, nonzero. */
    uint64_t lease_epoch; /**< Input lease the press belongs to. `drop_presses` keeps one epoch. */
    uint32_t kind; /**< A `host_wayland_press_kind_t` value. */
    uint32_t button; /**< Evdev button or key code; nonzero for a pointer press. */
    uint64_t now_ms; /**< Monotonic clock in milliseconds; see `host_wayland_windows_record_press`. */
} host_wayland_press_input_t;

/** @brief Compositor event sinks. Any member may be NULL; the event is then dropped. */
typedef struct host_wayland_callbacks_t {
    void *user; /**< Passed unchanged to every callback. Borrowed. */
    /** xdg_toplevel.configure: `state_bits` is an `interop_window_state_t` mask without Minimized; 0 width or height means the client chooses. */
    void (*on_configure)(void *user, uint64_t window, uint64_t generation, uint32_t state_bits, uint32_t width, uint32_t height);
    /** xdg_toplevel.close: the compositor asked the window to close. */
    void (*on_close)(void *user, uint64_t window, uint64_t generation);
    /** xdg_popup.configure, converted to guest pixels in the direct parent's coordinates (PopupRepositioned). */
    void (*on_popup_repositioned)(void *user, uint64_t window, uint64_t generation, int32_t x, int32_t y, uint32_t width, uint32_t height);
    /** xdg_popup.popup_done, after the popup was marked dismissed in the table. */
    void (*on_popup_dismissed)(void *user, uint64_t window, uint64_t generation);
    /** A refused or failed frame: `refused_type` is the InteropMsg type and `error_code` an interop_error_t value. */
    void (*on_error)(void *user, uint16_t refused_type, uint32_t error_code, uint64_t window, uint64_t generation);
} host_wayland_callbacks_t;

/** @brief Configuration copied into the context by `host_wayland_windows_init`. */
typedef struct host_wayland_config_t {
    struct wl_compositor *compositor; /**< Required, borrowed. */
    struct xdg_wm_base *wm_base; /**< Required, borrowed. Its bound version gates set_reactive (>= 3). */
    uint32_t wm_base_version; /**< Bound xdg_wm_base version, at least 1. */
    struct zxdg_decoration_manager_v1 *decoration_manager; /**< Borrowed; NULL when the compositor lacks the manager, and then no window gets a decoration. */
    const char *device; /**< Borrowed NUL-terminated device name, 1..64 bytes, used in app ids `waddle.<device>.<app_key>`. */
    uint32_t scale_numerator; /**< Host scale fraction numerator; 0 with a zero denominator means 1/1. */
    uint32_t scale_denominator; /**< Host scale fraction denominator. */
    host_wayland_callbacks_t callbacks; /**< Event sinks. */
} host_wayland_config_t;

/** @brief Object kinds that carry a listener binding. */
typedef enum host_wayland_kind_t {
    HostWaylandKindXdgSurface = 0, /**< xdg_surface events. */
    HostWaylandKindToplevel = 1, /**< xdg_toplevel events. */
    HostWaylandKindPopup = 2, /**< xdg_popup events. */
    HostWaylandKindDecoration = 3, /**< zxdg_toplevel_decoration_v1 events. */
    HostWaylandKindCount = 4 /**< Number of kinds. */
} host_wayland_kind_t;

/**
 * @brief Listener context for one object. It lives inside the context, so its address is stable.
 *
 * A callback is valid only while `owner` is set, the record's generation equals `generation`, and the
 * proxy the event names is the record's current object of `kind`. Otherwise the event is a no-op.
 */
typedef struct host_wayland_binding_t {
    struct host_wayland_t *owner; /**< Context that owns the record, NULL when unused. */
    uint64_t generation; /**< Generation the binding was made for. */
    uint32_t slot; /**< Record index. */
    uint32_t kind; /**< A `host_wayland_kind_t` value. */
} host_wayland_binding_t;

/** @brief One realized window and the Wayland objects that realize it. Owned by the context. */
typedef struct host_wayland_record_t {
    uint64_t window; /**< Protocol window handle; meaningful while `generation` is nonzero. */
    uint64_t generation; /**< Protocol generation; zero for a free record. */
    uint64_t parent; /**< Requested direct parent window, zero for none. */
    uint64_t parent_generation; /**< Requested direct parent generation. */
    uint8_t role; /**< Raw `interop_window_role_t`. */
    uint8_t mapped; /**< Nonzero after the first xdg_surface.configure was acknowledged. */
    uint8_t dismissed; /**< Nonzero once a popup reported popup_done. */
    uint8_t grabbed; /**< Nonzero while this popup holds an xdg_popup grab on `grab_seat` (3.6.2). */
    struct wl_seat *grab_seat; /**< Seat of the grab; meaningful while `grabbed` is nonzero. */
    uint64_t grab_sequence; /**< Grab order on the context; the highest live sequence of a seat is its topmost grab. */
    uint32_t reposition_token; /**< Token of the last xdg_popup.reposition sent; zero before the first. */
    uint8_t configure_latched; /**< Nonzero from an xdg_toplevel.configure until the acknowledging xdg_surface.configure (4.5). */
    uint32_t latched_bits; /**< Latched `interop_window_state_t` bits. */
    uint32_t latched_width; /**< Latched width, zero when the compositor lets the client decide. */
    uint32_t latched_height; /**< Latched height, zero when the compositor lets the client decide. */
    uint8_t reposition_pending; /**< Nonzero from a reposition request until its token is acknowledged. */
    uint32_t decoration_pending; /**< Staged decoration mode, zero when nothing is staged. */
    uint32_t decoration_applied; /**< Last applied decoration mode, zero before the first surface configuration. */
    int32_t guest_x; /**< Guest rectangle, most recent announce or update. */
    int32_t guest_y; /**< Guest rectangle. */
    uint32_t guest_width; /**< Guest rectangle. */
    uint32_t guest_height; /**< Guest rectangle. */
    uint32_t scale; /**< Host integer scale used when the window was realized. */
    host_popup_request_t request; /**< Popup positioner values; meaningful for popup roles. */
    uint16_t title_length; /**< Bytes stored in `title`. */
    char title[1025]; /**< Last title sent to set_title, NUL-terminated. */
    struct wl_surface *surface; /**< Owned wl_surface, NULL when absent. */
    struct xdg_surface *xdg_surface; /**< Owned xdg_surface, NULL when absent. */
    struct xdg_toplevel *toplevel; /**< Owned xdg_toplevel, NULL unless a toplevel role. */
    struct xdg_popup *popup; /**< Owned xdg_popup, NULL unless a popup role. */
    struct xdg_positioner *positioner; /**< Owned positioner while the popup is being created; NULL afterwards. */
    struct zxdg_toplevel_decoration_v1 *decoration; /**< Owned decoration, NULL when absent or destroyed. */
    host_wayland_binding_t bindings[HostWaylandKindCount]; /**< Listener contexts, one per kind. */
} host_wayland_record_t;

/** @brief One press-table entry (3.6.2): a physical press that may later justify a popup grab. */
typedef struct host_wayland_press_t {
    uint32_t protocol_serial; /**< Host protocol input serial T; never a Wayland serial. */
    uint32_t wayland_serial; /**< Wayland seat serial S, resolved only at a compositor request. */
    struct wl_seat *seat; /**< Borrowed seat the press came from. */
    uint64_t window; /**< Window the press landed on. */
    uint64_t generation; /**< Generation of that window. */
    uint64_t recorded_ms; /**< Caller clock at record time, milliseconds. */
    uint64_t lease_epoch; /**< Input lease of the press. */
    uint32_t kind; /**< A `host_wayland_press_kind_t` value. */
    uint32_t button; /**< Evdev code; nonzero for a pointer press. */
    uint64_t sequence; /**< Record order on the context; the highest live sequence of a window is its latest press. */
    uint8_t live; /**< Nonzero for a used entry. */
} host_wayland_press_t;

/** @brief Context storage. Private fields; see the ownership rules above. */
typedef struct host_wayland_t {
    host_wayland_config_t config; /**< Copied configuration. */
    uint32_t scale; /**< Integer host scale 1..8 used for min sizes and positioners. */
    uint8_t initialized; /**< Nonzero between init and reset. */
    host_window_map_t map; /**< Window table; each realized window has one record at its slot. */
    host_wayland_record_t records[HostWindowMapMaxRealized]; /**< Object records, indexed by the table's slot. */
    host_wayland_press_t presses[64]; /**< Bounded press table. */
    uint64_t grab_counter; /**< Sequence source for `grab_sequence`. */
    uint64_t last_clock_ms; /**< Last clock value seen; a smaller one clears the press table. */
    uint64_t press_sequence; /**< Sequence source for `sequence` of recorded presses. */
    struct wl_seat *seats[HostWaylandMaxSeats]; /**< Registered seats; a grab needs its press seat here. */
} host_wayland_t;

/** @brief Length of the press-table validity window, milliseconds (3.6.2 "300 ms rule"). */
#define HostWaylandPressExpiryMs 300u

/** @brief Number of realized windows the table can hold (`host_window_map.h` MaxRealized). */
#define HostWaylandMaxWindows 64u

/**
 * @brief Initialize a context and copy its configuration.
 *
 * @param ctx Out. Caller-owned storage, overwritten on success. NULL gives HostWaylandInvalidArgument.
 * @param config In. Non-NULL. The compositor, wm_base and device name are required; the device name is 1..64 bytes;
 *               wm_base_version is at least 1; the scale fraction must give an integer scale of 1..8 (a zero pair
 *               means 1/1).
 * @return HostWaylandOk; HostWaylandInvalidArgument for a NULL pointer, a missing required member, a bad device
 *         length, a zero wm_base_version, or an invalid scale fraction. Nothing is stored on failure. Thread safety:
 *         owner thread.
 */
host_wayland_status_t host_wayland_windows_init(host_wayland_t *ctx, const host_wayland_config_t *config);

/**
 * @brief Epoch reset: destroy every realized window in teardown order and clear the press table, seat registry and
 * grab state.
 *
 * Teardown is the table's own reset order (children first; roots newest first). Each window's objects are
 * destroyed decoration first, then toplevel or popup, then xdg_surface, then wl_surface. A corrupt table is still
 * torn down: its records are destroyed by descending slot index, with the same per-window order.
 *
 * @param ctx In/out. Initialized context. After success the context is uninitialized until
 *            `host_wayland_windows_init` runs again.
 * @return HostWaylandOk; HostWaylandNotInitialized for NULL or an uninitialized context. Thread safety: owner thread.
 */
host_wayland_status_t host_wayland_windows_reset(host_wayland_t *ctx);

/**
 * @brief Apply one guest frame: WindowAnnounce, WindowUpdate or WindowRetire.
 *
 * @param ctx In/out. Initialized context.
 * @param frame In. Borrowed frame bytes: a complete header followed by its payload.
 * @param length In. Bytes available in `frame`.
 * @param now_ms In. Monotonic clock in milliseconds. A value below the last one seen clears the press table
 *               (see `host_wayland_windows_record_press`).
 * @return HostWaylandOk; HostWaylandInvalidArgument for a NULL context or frame; HostWaylandNotInitialized;
 *         HostWaylandMalformedFrame for a bad header, payload or field; HostWaylandUnsupportedType for an unknown
 *         type or a host-to-guest type; HostWaylandRefused when the table or the positioner refused the frame
 *         (`on_error` reported); HostWaylandRealizeFailed when a Wayland object or listener could not be created (the
 *         window was rolled back and `on_error` reported). A stale window pair is HostWaylandOk and reports nothing.
 */
host_wayland_status_t host_wayland_windows_handle_frame(host_wayland_t *ctx, const uint8_t *frame, size_t length, uint64_t now_ms);

/**
 * @brief Whether a realized window has been acknowledged as configured (the "mapped" state of 4.3).
 *
 * @param ctx In. Initialized context; NULL or uninitialized gives false.
 * @param window In. Protocol window handle.
 * @param generation In. Protocol generation.
 * @return True after the first xdg_surface.configure was acknowledged, while the window stays realized; false otherwise.
 */
bool host_wayland_windows_is_mapped(const host_wayland_t *ctx, uint64_t window, uint64_t generation);

/**
 * @brief Record a physical press that may justify a later popup grab (3.6.2).
 *
 * Entries older than HostWaylandPressExpiryMs are removed first. A full table of live entries is never evicted.
 * The clock is the caller's monotonic clock in milliseconds (for example CLOCK_MONOTONIC). If it ever moves
 * backwards, every stored press is untrustworthy, so the table is cleared before the new press is recorded.
 * The seat is registered on success; a press on a seat that was forgotten cannot be recorded until it is
 * registered again by a new press.
 *
 * @param ctx In/out. Initialized context.
 * @param input In. The press. NULL, a zero protocol serial, a NULL seat, a zero window or generation, an unknown
 *              kind, or a pointer press with a zero button gives HostWaylandInvalidArgument.
 * @return HostWaylandOk; HostWaylandNotInitialized; HostWaylandInvalidArgument; HostWaylandDuplicateSerial when a
 *         live press holds the same T; HostWaylandPressTableFull when all 64 entries are live (the caller must
 *         fail the session, 3.6.2); or HostWaylandSeatTableFull when no seat slot is free.
 */
host_wayland_status_t host_wayland_windows_record_press(host_wayland_t *ctx, const host_wayland_press_input_t *input);

/**
 * @brief Remove the press entry for protocol serial T (release or cancel). Unknown serials are ignored.
 *
 * @param ctx In/out. Initialized context; NULL or uninitialized is a no-op.
 * @param protocol_serial In. Host protocol input serial T.
 */
void host_wayland_windows_release_press(host_wayland_t *ctx, uint32_t protocol_serial);

/**
 * @brief Keep only the presses of one input lease. Every other press is dropped: focus loss or a lease change
 * makes its serials invalid for a grab.
 *
 * @param ctx In/out. Initialized context; NULL or uninitialized is a no-op.
 * @param lease_epoch In. The lease epoch that stays valid.
 */
void host_wayland_windows_drop_presses(host_wayland_t *ctx, uint64_t lease_epoch);

/**
 * @brief Forget a seat that was destroyed. Its presses are dropped, and every popup grab on it is released so
 * that no later popup can be granted on the destroyed seat.
 *
 * @param ctx In/out. Initialized context; NULL or uninitialized is a no-op.
 * @param seat In. The destroyed seat; NULL is a no-op. The module never dereferences it.
 */
void host_wayland_windows_forget_seat(host_wayland_t *ctx, const struct wl_seat *seat);

/**
 * @brief Change the host integer scale for windows realized from now on. Realized windows keep the scale they were
 * realized with.
 *
 * @param ctx In/out. Initialized context.
 * @param numerator In. Fraction numerator; the integer scale is `ceil(numerator / denominator)`, 1..8.
 * @param denominator In. Fraction denominator, nonzero.
 * @return HostWaylandOk; HostWaylandInvalidArgument for a zero denominator or an integer scale outside 1..8;
 *         HostWaylandNotInitialized for NULL or an uninitialized context.
 */
host_wayland_status_t host_wayland_windows_set_scale(host_wayland_t *ctx, uint32_t numerator, uint32_t denominator);

/**
 * @brief Re-send the parent of every realized child dialog of a window that has just mapped.
 *
 * A dialog's xdg_toplevel.set_parent names the parent's toplevel, and the compositor only honours it once that parent
 * has a buffer. The frames layer calls this on the window's first buffer commit, so each child dialog repeats its
 * set_parent after the parent is visible. The initial set_parent at realization is kept.
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle of the window that mapped; zero gives HostWaylandInvalidArgument.
 * @param generation In. Its generation; zero gives HostWaylandInvalidArgument.
 * @return HostWaylandOk, including when the window is not realized as a toplevel (a stale call is a no-op), or
 *         HostWaylandNotInitialized. Thread safety: owner thread, synchronous.
 */
host_wayland_status_t host_wayland_windows_note_mapped(host_wayland_t *ctx, uint64_t window, uint64_t generation);

/** @brief What a live press records, for the session handler's validation of guest requests (4.5). */
typedef struct host_wayland_press_info_t {
    struct wl_seat *seat; /**< Seat the press came from; may be unregistered since, check before use. */
    uint32_t wayland_serial; /**< Wayland seat serial S to pass to a compositor request. */
    uint64_t window; /**< Window the press landed on. */
    uint64_t generation; /**< Its generation. */
    uint64_t lease_epoch; /**< Input lease of the press. */
    uint32_t kind; /**< A `host_wayland_press_kind_t` value. */
    uint32_t button; /**< Evdev code; nonzero for a pointer press. */
    uint8_t latest_for_window; /**< Nonzero when no later live press holds the same window and generation. */
} host_wayland_press_info_t;

/**
 * @brief Look up the live press for protocol serial T at `now_ms`, with its window and latest-press status.
 *
 * @param ctx In/out. Initialized context; expired entries are removed first.
 * @param protocol_serial In. Host protocol serial T named by a guest request.
 * @param now_ms In. Monotonic clock in milliseconds.
 * @param out In/out. Written only on HostWaylandOk.
 * @return HostWaylandOk; HostWaylandPressNotFound when no live entry holds T; HostWaylandInvalidArgument for NULL
 *         context or output, or a zero serial; HostWaylandNotInitialized. Thread safety: owner thread.
 */
host_wayland_status_t host_wayland_windows_lookup_press(host_wayland_t *ctx, uint32_t protocol_serial, uint64_t now_ms, host_wayland_press_info_t *out);

/**
 * @brief Start an interactive move of a realized toplevel (xdg_toplevel.move, 4.5).
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle; zero gives InvalidArgument.
 * @param generation In. Its generation; zero gives InvalidArgument.
 * @param seat In. Borrowed seat; must be registered (see record_press), otherwise Refused.
 * @param wayland_serial In. Wayland seat serial S of the press that justifies the move.
 * @return HostWaylandOk; Refused when the window is not realized as a toplevel or the seat is not registered;
 *         InvalidArgument; NotInitialized. Thread safety: owner thread.
 */
host_wayland_status_t host_wayland_windows_begin_move(host_wayland_t *ctx, uint64_t window, uint64_t generation, struct wl_seat *seat, uint32_t wayland_serial);

/**
 * @brief Start an interactive resize from one edge (xdg_toplevel.resize, 4.5).
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle.
 * @param generation In. Its generation.
 * @param seat In. Borrowed seat; must be registered, otherwise Refused.
 * @param wayland_serial In. Wayland seat serial S of the press.
 * @param edge In. An `interop_resize_edge_t` value: 1, 2, 4, 5, 6, 8, 9 or 10 (the xdg-shell edge values); other
 *             values are InvalidArgument.
 * @return HostWaylandOk; Refused; InvalidArgument; NotInitialized.
 */
host_wayland_status_t host_wayland_windows_begin_resize(host_wayland_t *ctx, uint64_t window, uint64_t generation, struct wl_seat *seat, uint32_t wayland_serial, uint32_t edge);

/**
 * @brief Set or clear the compositor's maximized state of a realized toplevel (xdg_toplevel.set_maximized or
 * unset_maximized). Persistent host state: the reducer decides when this is requested.
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle.
 * @param generation In. Its generation.
 * @param maximized In. True requests maximize, false requests unmaximize.
 * @return HostWaylandOk; Refused when the window is not realized as a toplevel; InvalidArgument; NotInitialized.
 */
host_wayland_status_t host_wayland_windows_set_maximized(host_wayland_t *ctx, uint64_t window, uint64_t generation, bool maximized);

/**
 * @brief Set or clear fullscreen on a realized toplevel (xdg_toplevel.set_fullscreen with NULL output, or
 * unset_fullscreen).
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle.
 * @param generation In. Its generation.
 * @param fullscreen In. True enters fullscreen, false leaves it.
 * @return HostWaylandOk; Refused; InvalidArgument; NotInitialized.
 */
host_wayland_status_t host_wayland_windows_set_fullscreen(host_wayland_t *ctx, uint64_t window, uint64_t generation, bool fullscreen);

/**
 * @brief Minimize a realized toplevel (xdg_toplevel.set_minimized; one-shot, xdg-shell has no minimized state).
 *
 * @param ctx In/out. Initialized context.
 * @param window In. Protocol window handle.
 * @param generation In. Its generation.
 * @return HostWaylandOk; Refused; InvalidArgument; NotInitialized.
 */
host_wayland_status_t host_wayland_windows_set_minimized(host_wayland_t *ctx, uint64_t window, uint64_t generation);

/**
 * @brief Whether a window pair is realized in the table right now.
 *
 * @param ctx In. Initialized context; NULL or uninitialized gives false.
 * @param window In. Protocol window handle.
 * @param generation In. Its generation.
 * @return True when realized exactly.
 */
bool host_wayland_windows_contains(const host_wayland_t *ctx, uint64_t window, uint64_t generation);

/**
 * @brief Number of realized windows.
 *
 * @param ctx In. Initialized context; NULL or uninitialized gives 0.
 * @return The table's realized count, 0 to HostWindowMapMaxRealized.
 */
uint32_t host_wayland_windows_window_count(const host_wayland_t *ctx);

_Static_assert(HostWaylandMaxWindows == HostWindowMapMaxRealized, "record table matches the window table");

#endif
