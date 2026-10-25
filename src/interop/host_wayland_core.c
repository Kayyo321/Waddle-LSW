/**
 * @file host_wayland_core.c
 * @brief Implementation of the session Wayland connection declared in host_wayland_core.h.
 *
 * Single thread only. Every libwayland call is made by the thread that owns the core, and every listener callback
 * runs inside one of the dispatch calls made here. The core never allocates.
 */
#include "host_wayland_core.h"

#include "wayland-client-core.h"
#include "wayland-client-protocol.h"
#include "xdg_decoration_client.h"
#include "xdg_shell_client.h"

#include <errno.h>
#include <string.h>

/** @brief Highest wl_compositor version bound. wl_compositor has no destroy request at this version. */
#define HostWaylandCoreCompositorMaxVersion 4u
/** @brief Highest xdg_wm_base version bound. */
#define HostWaylandCoreWmBaseMaxVersion 5u
/** @brief Highest wl_shm version bound. Version 1 has no release request, so shm is torn down client-side. */
#define HostWaylandCoreShmMaxVersion 1u
/** @brief Highest zxdg_decoration_manager_v1 version bound. */
#define HostWaylandCoreDecorationMaxVersion 1u
/** @brief Highest wl_seat version bound. */
#define HostWaylandCoreSeatMaxVersion 7u
/** @brief First wl_seat version whose release request exists. Older seats are destroyed client-side. */
#define HostWaylandCoreSeatReleaseVersion 5u

/**
 * @brief Smaller of two unsigned values.
 * @param a In. Any value.
 * @param b In. Any value.
 * @return `a` when `a <= b`, otherwise `b`.
 */
static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/**
 * @brief Mark the connection fatally failed. Sticky until host_wayland_core_close.
 * @param core In/out. Initialized core.
 * @return HostWaylandCoreDisplayError.
 */
static host_wayland_core_status_t fail_display(host_wayland_core_t *core) {
    core->failed = 1;
    return HostWaylandCoreDisplayError;
}

/**
 * @brief Answer an xdg_wm_base ping. The pong is queued here and sent by the flush that ends the dispatch.
 * @param data In. Unused; the core pointer.
 * @param wm_base In. The xdg_wm_base that sent the ping.
 * @param serial In. Ping serial, echoed unchanged.
 */
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

/** @brief xdg_wm_base listener: only ping is used. */
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/**
 * @brief Bind xdg_wm_base and register the ping listener. A wm_base whose listener cannot be added is destroyed and
 * not recorded, so startup reports MissingGlobal rather than running without ping answers.
 * @param core In/out. Core whose wm_base slot is filled.
 * @param registry In. Registry that announced the global.
 * @param name In. Registry name of the global.
 * @param announced In. Version the compositor announced.
 */
static void bind_wm_base(host_wayland_core_t *core, struct wl_registry *registry, uint32_t name, uint32_t announced) {
    if (core->globals.wm_base) return;
    uint32_t version = min_u32(announced, HostWaylandCoreWmBaseMaxVersion);
    struct xdg_wm_base *wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
    if (!wm_base) return;
    if (xdg_wm_base_add_listener(wm_base, &wm_base_listener, core) != 0) {
        xdg_wm_base_destroy(wm_base);
        return;
    }
    core->globals.wm_base = wm_base;
    core->globals.wm_base_version = version;
}

/**
 * @brief Record a newly announced wl_seat in the first free slot. A full table refuses the seat before binding it.
 * @param core In/out. Core whose seat table is updated; the seat listener, if set, is called with added=true.
 * @param registry In. Registry that announced the seat.
 * @param name In. Registry name of the seat; kept for global_remove.
 * @param announced In. Version the compositor announced.
 */
static void record_seat(host_wayland_core_t *core, struct wl_registry *registry, uint32_t name, uint32_t announced) {
    uint32_t slot = HostWaylandCoreMaxSeats;
    for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
        if (!core->seats[index]) {
            slot = index;
            break;
        }
    }
    if (slot == HostWaylandCoreMaxSeats) {
        ++core->seats_refused;
        return;
    }
    struct wl_seat *seat = wl_registry_bind(registry, name, &wl_seat_interface, min_u32(announced, HostWaylandCoreSeatMaxVersion));
    if (!seat) return;
    core->seats[slot] = seat;
    core->seat_names[slot] = name;
    if (core->seat_listener) core->seat_listener(core->seat_user, seat, true);
}

/**
 * @brief Destroy a seat proxy. Seats at version 5 and later are released with the request; older seats only lose
 * the local proxy because their protocol has no release request.
 * @param seat In. Seat proxy, consumed by this call.
 */
static void destroy_seat(struct wl_seat *seat) {
    if (wl_proxy_get_version((struct wl_proxy *)seat) >= HostWaylandCoreSeatReleaseVersion) {
        wl_seat_release(seat);
    } else {
        wl_seat_destroy(seat);
    }
}

/**
 * @brief Remove the seat in one table slot: notify the listener while the pointer is valid, destroy it, free the slot.
 * @param core In/out. Core owning the slot.
 * @param index In. Slot index, below HostWaylandCoreMaxSeats, holding a recorded seat.
 */
static void remove_seat_at(host_wayland_core_t *core, uint32_t index) {
    struct wl_seat *seat = core->seats[index];
    if (core->seat_listener) core->seat_listener(core->seat_user, seat, false);
    destroy_seat(seat);
    core->seats[index] = NULL;
    core->seat_names[index] = 0;
}

/**
 * @brief Registry listener: a global was announced. Binds the globals this core uses and ignores the rest.
 * @param data In. The core.
 * @param registry In. Registry that announced the global.
 * @param name In. Registry name.
 * @param interface In. Interface name; NUL terminated.
 * @param version In. Version the compositor offers.
 */
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    host_wayland_core_t *core = data;
    if (!strcmp(interface, "wl_compositor")) {
        if (!core->globals.compositor) {
            core->globals.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_u32(version, HostWaylandCoreCompositorMaxVersion));
        }
    } else if (!strcmp(interface, "xdg_wm_base")) {
        bind_wm_base(core, registry, name, version);
    } else if (!strcmp(interface, "wl_shm")) {
        if (!core->globals.shm) {
            core->globals.shm = wl_registry_bind(registry, name, &wl_shm_interface, min_u32(version, HostWaylandCoreShmMaxVersion));
        }
    } else if (!strcmp(interface, "zxdg_decoration_manager_v1")) {
        if (!core->globals.decoration_manager) {
            core->globals.decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, min_u32(version, HostWaylandCoreDecorationMaxVersion));
        }
    } else if (!strcmp(interface, "wl_seat")) {
        record_seat(core, registry, name, version);
    }
}

/**
 * @brief Registry listener: a global was removed. Only recorded seats are acted on; other globals are not tracked.
 * @param data In. The core.
 * @param registry In. Unused.
 * @param name In. Registry name that went away.
 */
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    host_wayland_core_t *core = data;
    (void)registry;
    for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
        if (core->seats[index] && core->seat_names[index] == name) remove_seat_at(core, index);
    }
}

/** @brief Registry listener table. */
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/**
 * @brief Flush queued requests and update the POLLOUT state.
 * @param core In/out. Initialized, non-failed core.
 * @return HostWaylandCoreOk when the queue drained; WouldBlock on EAGAIN with no display error; DisplayError for any
 *         other failure, or when wl_display_get_error is nonzero.
 */
static host_wayland_core_status_t flush_output(host_wayland_core_t *core) {
    if (wl_display_flush(core->display) >= 0) {
        core->output_pending = 0;
        return HostWaylandCoreOk;
    }
    int saved = errno;
    if (saved == EAGAIN && wl_display_get_error(core->display) == 0) {
        core->output_pending = 1;
        return HostWaylandCoreWouldBlock;
    }
    return fail_display(core);
}

/**
 * @brief Make the default queue empty so that a read intention can be prepared.
 *
 * A failed prepare with EAGAIN means events are queued; they are dispatched and the prepare is retried. A failed
 * prepare with any other errno, or with nothing left to dispatch, is treated as a broken connection.
 * @param display In/out. Connection with no read intention outstanding.
 * @return true when a read intention is prepared; false when the connection cannot make progress.
 */
static bool drain_before_read(struct wl_display *display) {
    while (wl_display_prepare_read(display) != 0) {
        if (errno != EAGAIN) return false;
        if (wl_display_dispatch_pending(display) <= 0) return false;
    }
    return true;
}

/**
 * @brief Read the socket and dispatch what arrived.
 *
 * A read that fails with EAGAIN or EINTR, and no display error, is not fatal: nothing was read. Any other read
 * failure, a nonzero display error, or a failed dispatch is fatal.
 * @param display In/out. Connection.
 * @return true when the read and the following dispatch completed; false when the connection is fatally broken.
 */
static bool read_socket_events(struct wl_display *display) {
    if (!drain_before_read(display)) return false;
    if (wl_display_read_events(display) < 0) {
        int saved = errno;
        if (wl_display_get_error(display) != 0) return false;
        if (saved != EAGAIN && saved != EINTR) return false;
    }
    return wl_display_dispatch_pending(display) >= 0;
}

host_wayland_core_status_t host_wayland_core_init(host_wayland_core_t *core, const host_wayland_core_config_t *config) {
    if (!core || !config || core->initialized) return HostWaylandCoreInvalidArgument;
    struct wl_display *display = config->display;
    uint8_t owns_display = 0;
    if (!display) {
        display = wl_display_connect(config->display_name);
        if (!display) return HostWaylandCoreConnectFailed;
        owns_display = 1;
    }
    memset(core, 0, sizeof *core);
    core->display = display;
    core->owns_display = owns_display;
    core->initialized = 1;
    core->registry = wl_display_get_registry(display);
    if (!core->registry || wl_registry_add_listener(core->registry, &registry_listener, core) != 0) {
        host_wayland_core_close(core);
        return HostWaylandCoreRoundtripFailed;
    }
    if (wl_display_roundtrip(display) < 0) {
        host_wayland_core_close(core);
        return HostWaylandCoreRoundtripFailed;
    }
    if (!core->globals.compositor || !core->globals.wm_base || !core->globals.shm) {
        host_wayland_core_close(core);
        return HostWaylandCoreMissingGlobal;
    }
    return HostWaylandCoreOk;
}

void host_wayland_core_close(host_wayland_core_t *core) {
    if (!core || !core->initialized) return;
    for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
        if (core->seats[index]) remove_seat_at(core, index);
    }
    if (core->globals.decoration_manager) zxdg_decoration_manager_v1_destroy(core->globals.decoration_manager);
    if (core->globals.shm) wl_shm_destroy(core->globals.shm);
    if (core->globals.wm_base) xdg_wm_base_destroy(core->globals.wm_base);
    if (core->globals.compositor) wl_compositor_destroy(core->globals.compositor);
    if (core->registry) wl_registry_destroy(core->registry);
    if (core->owns_display) wl_display_disconnect(core->display);
    memset(core, 0, sizeof *core);
}

short host_wayland_core_poll_events(const host_wayland_core_t *core) {
    if (!core || !core->initialized || core->failed) return 0;
    return (short)(POLLIN | (core->output_pending ? POLLOUT : 0));
}

int host_wayland_core_fd(const host_wayland_core_t *core) {
    if (!core || !core->initialized || core->failed) return -1;
    return wl_display_get_fd(core->display);
}

host_wayland_core_status_t host_wayland_core_dispatch(host_wayland_core_t *core, short revents) {
    if (!core) return HostWaylandCoreInvalidArgument;
    if (!core->initialized) return HostWaylandCoreNotInitialized;
    if (core->failed) return HostWaylandCoreDisplayError;
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) return fail_display(core);
    if (wl_display_dispatch_pending(core->display) < 0) return fail_display(core);
    if ((revents & POLLIN) && !read_socket_events(core->display)) return fail_display(core);
    return flush_output(core);
}

host_wayland_core_status_t host_wayland_core_flush(host_wayland_core_t *core) {
    if (!core) return HostWaylandCoreInvalidArgument;
    if (!core->initialized) return HostWaylandCoreNotInitialized;
    if (core->failed) return HostWaylandCoreDisplayError;
    return flush_output(core);
}

host_wayland_core_status_t host_wayland_core_roundtrip(host_wayland_core_t *core) {
    if (!core) return HostWaylandCoreInvalidArgument;
    if (!core->initialized) return HostWaylandCoreNotInitialized;
    if (core->failed) return HostWaylandCoreDisplayError;
    if (wl_display_roundtrip(core->display) < 0) {
        if (wl_display_get_error(core->display) != 0) return fail_display(core);
        return HostWaylandCoreRoundtripFailed;
    }
    core->output_pending = 0;
    return HostWaylandCoreOk;
}

host_wayland_core_status_t host_wayland_core_set_seat_listener(host_wayland_core_t *core, host_wayland_core_seat_listener_t listener, void *user) {
    if (!core) return HostWaylandCoreInvalidArgument;
    if (!core->initialized) return HostWaylandCoreNotInitialized;
    core->seat_listener = listener;
    core->seat_user = user;
    if (listener) {
        for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
            if (core->seats[index]) listener(user, core->seats[index], true);
        }
    }
    return HostWaylandCoreOk;
}

const host_wayland_core_globals_t *host_wayland_core_globals(const host_wayland_core_t *core) {
    if (!core || !core->initialized) return NULL;
    return &core->globals;
}

uint32_t host_wayland_core_seat_count(const host_wayland_core_t *core) {
    if (!core || !core->initialized) return 0;
    uint32_t count = 0;
    for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
        if (core->seats[index]) ++count;
    }
    return count;
}

struct wl_seat *host_wayland_core_seat_at(const host_wayland_core_t *core, uint32_t index) {
    if (!core || !core->initialized || index >= HostWaylandCoreMaxSeats) return NULL;
    return core->seats[index];
}
