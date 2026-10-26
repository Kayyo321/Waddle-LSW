#ifndef WaddleHostWaylandCoreH
#define WaddleHostWaylandCoreH
/**
 * @file host_wayland_core.h
 * @brief Session-owned Wayland connection: display, registry globals, seats and the poll loop (TODO #11g, #13).
 *
 * One core owns the wl_display connection of one waddle-session run. It binds the globals the windows layer needs,
 * records the wl_seat objects so the input lane can observe them, answers xdg_wm_base pings, and exposes one poll
 * descriptor with the events the caller must multiplex. The windows layer (host_wayland_windows.h) and the session
 * handler (session_windows_handler.h) use the globals through the accessors below. This file never creates
 * windows.
 *
 * Ownership and threading:
 *  - The caller owns the host_wayland_core_t storage. It must not move while initialized and must be closed before
 *    its storage is released. Fields are private.
 *  - The display connection is owned by the core unless `config.display` is supplied; an injected display is
 *    borrowed and never disconnected. The Wayland objects the core binds are owned by the core and destroyed by
 *    host_wayland_core_close (see the contract for the order).
 *  - Single-thread only: the thread that calls init also calls every other function and receives every callback.
 *    Callbacks run synchronously inside host_wayland_core_dispatch or host_wayland_core_roundtrip.
 *  - The core never allocates. Seat storage is a fixed table of HostWaylandCoreMaxSeats entries.
 *
 * Failure model: a display error (protocol error, EPIPE, a failed read, or POLLERR/POLLHUP on the descriptor) is
 * fatal for the connection. The core reports HostWaylandCoreDisplayError and then refuses every further dispatch,
 * flush and roundtrip until it is closed. A transient EAGAIN on read or flush is not an error.
 */
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wl_compositor;
struct wl_seat;
struct wl_shm;
struct xdg_wm_base;
struct zxdg_decoration_manager_v1;

/** @brief Seats the core records at once. A seat announced beyond this is refused and counted. */
#define HostWaylandCoreMaxSeats 8u

/** @brief Result of every core function. */
typedef enum host_wayland_core_status_t {
    HostWaylandCoreOk = 0, /**< The call completed. */
    HostWaylandCoreInvalidArgument = 1, /**< NULL storage or config, or an out-of-range argument. Nothing changed. */
    HostWaylandCoreConnectFailed = 2, /**< wl_display_connect failed (no display name resolved or no socket). */
    HostWaylandCoreMissingGlobal = 3, /**< A required global is absent: wl_compositor, xdg_wm_base or wl_shm. Connection closed. */
    HostWaylandCoreRoundtripFailed = 4, /**< The startup roundtrip did not complete. Connection closed. */
    HostWaylandCoreDisplayError = 5, /**< Fatal display error: the connection is unusable until closed. */
    HostWaylandCoreNotInitialized = 6, /**< The core is not initialized, or was closed. */
    HostWaylandCoreWouldBlock = 7 /**< A flush could not be completed; the caller must wait for POLLOUT. Not an error. */
} host_wayland_core_status_t;

/**
 * @brief Seat notification: `added` is true when a wl_seat was bound and recorded, false when the compositor removed
 * it. On removal the pointer is still valid during the call and is destroyed right after it returns.
 */
typedef void (*host_wayland_core_seat_listener_t)(void *user, struct wl_seat *seat, bool added);

/** @brief Startup configuration. */
typedef struct host_wayland_core_config_t {
    /** @brief Display socket name. NULL means WAYLAND_DISPLAY, resolved under XDG_RUNTIME_DIR by libwayland. Borrowed. */
    const char *display_name;
    /** @brief Injected display (tests). When non-NULL no connection is made and the display is borrowed. */
    struct wl_display *display;
} host_wayland_core_config_t;

/** @brief Globals bound at startup. Pointers are owned by the core and valid until close. NULL when absent. */
typedef struct host_wayland_core_globals_t {
    struct wl_compositor *compositor; /**< Required. */
    struct xdg_wm_base *wm_base; /**< Required. Answers pings automatically. */
    uint32_t wm_base_version; /**< Bound xdg_wm_base version, at least 1 when wm_base is set. */
    struct zxdg_decoration_manager_v1 *decoration_manager; /**< Optional; NULL when the compositor lacks it. */
    struct wl_shm *shm; /**< Required; bound for the frames layer. */
} host_wayland_core_globals_t;

/** @brief Connection state. Private fields; see the ownership rules above. */
typedef struct host_wayland_core_t {
    uint8_t initialized; /**< Nonzero between a successful init and close. */
    uint8_t failed; /**< Nonzero after a fatal display error. */
    uint8_t owns_display; /**< Nonzero when the core connected and must disconnect. */
    uint8_t output_pending; /**< Nonzero after a flush returned EAGAIN, until a flush or roundtrip completes. */
    struct wl_display *display; /**< The connection. */
    struct wl_registry *registry; /**< The registry the globals were announced on. */
    host_wayland_core_globals_t globals; /**< Bound globals. */
    struct wl_seat *seats[HostWaylandCoreMaxSeats]; /**< Recorded seats; NULL for a free entry. */
    uint32_t seat_names[HostWaylandCoreMaxSeats]; /**< Registry name of each recorded seat, for global_remove. */
    uint32_t seats_refused; /**< Seats announced while the table was full. */
    host_wayland_core_seat_listener_t seat_listener; /**< Optional seat notifications. */
    void *seat_user; /**< Passed unchanged to seat_listener. */
} host_wayland_core_t;

/**
 * @brief Connect, bind the globals and complete the startup roundtrip.
 *
 * Binds wl_compositor, xdg_wm_base, wl_shm, zxdg_decoration_manager_v1 (optional) and wl_seat. A wl_seat past
 * HostWaylandCoreMaxSeats is refused (counted in seats_refused), not an error.
 *
 * @param core In/out. Caller-owned storage, overwritten on success; left unchanged on failure except for the fields
 *             reset during teardown.
 * @param config In. Non-NULL.
 * @return HostWaylandCoreOk; InvalidArgument (NULL core or config, or a core that is already initialized and must be
 *         closed first); ConnectFailed; MissingGlobal (xdg_wm_base absent is a hard startup error, as are wl_compositor
 *         and wl_shm); RoundtripFailed. Every failure after a connection closes it again.
 *         Thread safety: owner thread.
 */
host_wayland_core_status_t host_wayland_core_init(host_wayland_core_t *core, const host_wayland_core_config_t *config);

/**
 * @brief Destroy the bound globals and seats, then the registry, then disconnect an owned display.
 *
 * Teardown order: recorded seats (wl_seat release), decoration manager, shm, xdg_wm_base, compositor, registry, then
 * the display. Every seat is reported to the seat listener as removed before its destruction.
 *
 * @param core In/out. NULL or uninitialized is a no-op. Afterwards the core is uninitialized.
 */
void host_wayland_core_close(host_wayland_core_t *core);

/**
 * @brief Events the caller must poll for: POLLIN always, POLLOUT only while the display has unflushed data.
 *
 * @param core In. Initialized core; NULL, uninitialized or failed gives 0.
 * @return The poll event mask for the descriptor from host_wayland_core_fd.
 */
short host_wayland_core_poll_events(const host_wayland_core_t *core);

/**
 * @brief The poll descriptor of the display (wl_display_get_fd). Borrowed; never closed by the caller.
 *
 * @param core In. Initialized core.
 * @return The descriptor, or -1 when the core is NULL, uninitialized or failed.
 */
int host_wayland_core_fd(const host_wayland_core_t *core);

/**
 * @brief Read and dispatch the events the poll reported, then flush pending requests.
 *
 * Sequence: dispatch already queued events; if POLLIN is set, read the socket and dispatch again; flush. POLLERR,
 * POLLHUP or POLLNVAL in revents, a dispatch or read failure other than EAGAIN/EINTR, a flush failure other than
 * EAGAIN, or a display error is fatal.
 *
 * @param core In/out. Initialized core.
 * @param revents In. The revents the poll returned for the core descriptor.
 * @return HostWaylandCoreOk; WouldBlock when output remains (wait for POLLOUT); DisplayError when fatal;
 *         NotInitialized; InvalidArgument for NULL. Thread safety: owner thread; callbacks run inside this call.
 */
host_wayland_core_status_t host_wayland_core_dispatch(host_wayland_core_t *core, short revents);

/**
 * @brief Flush queued requests, for example from a tick while POLLOUT is pending.
 *
 * @param core In/out. Initialized core.
 * @return HostWaylandCoreOk when nothing remains; WouldBlock when the socket is full; DisplayError when fatal;
 *         NotInitialized.
 */
host_wayland_core_status_t host_wayland_core_flush(host_wayland_core_t *core);

/**
 * @brief Synchronous roundtrip: dispatch until the compositor has processed everything sent so far.
 *
 * Used by tests and by the startup path. Callbacks run inside the call.
 *
 * @param core In/out. Initialized core.
 * @return HostWaylandCoreOk; RoundtripFailed or DisplayError; NotInitialized.
 */
host_wayland_core_status_t host_wayland_core_roundtrip(host_wayland_core_t *core);

/**
 * @brief Register the seat notification. Replaces any earlier listener. NULL removes it.
 *
 * Seats recorded before the listener was set are reported to it when it is set, so the input lane never misses one.
 *
 * @param core In/out. Initialized core.
 * @param listener In. Called on add and on removal; may be NULL.
 * @param user In. Passed unchanged to every call.
 * @return HostWaylandCoreOk or NotInitialized.
 */
host_wayland_core_status_t host_wayland_core_set_seat_listener(host_wayland_core_t *core, host_wayland_core_seat_listener_t listener, void *user);

/**
 * @brief Bound globals. The pointers are valid until host_wayland_core_close.
 *
 * @param core In. Initialized core.
 * @return The globals, or NULL when the core is NULL or uninitialized.
 */
const host_wayland_core_globals_t *host_wayland_core_globals(const host_wayland_core_t *core);

/**
 * @brief Number of recorded seats.
 *
 * @param core In. Initialized core; NULL or uninitialized gives 0.
 * @return Count in 0 .. HostWaylandCoreMaxSeats.
 */
uint32_t host_wayland_core_seat_count(const host_wayland_core_t *core);

/**
 * @brief Recorded seat at an index, for enumeration.
 *
 * @param core In. Initialized core.
 * @param index In. Index in 0 .. HostWaylandCoreMaxSeats - 1.
 * @return The seat in that slot, or NULL when the slot is free, the index is out of range, or the core is not initialized.
 */
struct wl_seat *host_wayland_core_seat_at(const host_wayland_core_t *core, uint32_t index);

#endif
