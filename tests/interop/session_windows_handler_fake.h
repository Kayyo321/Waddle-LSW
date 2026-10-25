/**
 * @file session_windows_handler_fake.h
 * @brief Combined recording double for the session windows handler test (session_windows_handler_test.c).
 *
 * One double serves the handler, the Wayland core and the window realization at once. It reuses the proxy and argument
 * recording of host_wayland_windows_fake.h (xdg objects, positioner, grab model, protocol error counters) and the
 * display, registry and event machinery of host_wayland_core_fake.h (wl_display_*, registry globals, roundtrip). The two
 * source doubles are not included and are not modified; they define the same __wrap symbols and cannot be combined.
 *
 * Model:
 *  - Every object is a fake proxy. Its address is a fixed slot in fake_storage. A destroyed object's slot goes on a
 *    free list and is handed to the next creation (last in, first out), as a real allocator would reuse it. Dead
 *    objects therefore keep their record until the slot is reused, and requests on them count as protocol errors.
 *  - wl_proxy_marshal_flags records every request: the proxy index, the interface, the opcode, whether it is a
 *    destroy, the decoded arguments (object arguments as proxy index, -1 for NULL, FakeNone for a foreign pointer),
 *    and the text of string arguments. A constructor creates its object with the interface and version it names.
 *  - Destroying a live object appends its interface name to fake_log, in destroy order. A destroy of a dead object is a
 *    double destroy. A destroy that leaves a live dependent behind is an order violation.
 *  - Compositor events are queued with the fake_queue_* functions and delivered in FIFO order by
 *    wl_display_dispatch_pending and wl_display_roundtrip, as libwayland delivers them. An event whose object is no longer
 *    live is dropped, as libwayland drops events after wl_proxy_destroy.
 *  - interop_host_feature_send is wrapped too. Its result is chosen by fake_send_mode; each accepted frame is copied into
 *    fake_frames in order.
 *
 * Protocol errors (every request on a dead or unknown proxy, a second role on an xdg_surface, an ack of a serial that was
 * not outstanding, an invalid grab, a listener installed twice) increment fake_protocol_errors. It is never reset within
 * a test; the test entry point requires it to stay zero.
 */
#ifndef SessionWindowsHandlerFakeH
#define SessionWindowsHandlerFakeH

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "session_control_host.h"
#include "wayland-client-core.h"
#include "wayland-client-protocol.h"
#include "xdg_decoration_client.h"
#include "xdg_shell_client.h"

/** @brief Proxy slots (also the storage slots). Dead slots are reused, so this bounds the live objects. */
#define FakeProxies 1024u
/** @brief Requests recorded between two fake_begin calls, or between two per-step resets. */
#define FakeRequests 8192u
/** @brief Queued compositor events waiting for the next dispatch. */
#define FakeEvents 512u
/** @brief Destroy log entries. */
#define FakeLog 2048u
/** @brief Accepted outbound frames kept by the sender double. */
#define FakeFrames 8192u
/** @brief Largest outbound frame the sender double keeps. */
#define FakeFrameBytes 256u
/** @brief Descriptor reported by wl_display_get_fd. */
#define FakeFd 7
/** @brief Configure serials remembered per xdg_surface. */
#define FakeSerials 8u
#define FakeNone 0xFFFFFFFFu

/** @brief Role an xdg_surface received; an xdg_surface takes at most one. */
enum fake_role_t {
    FakeRoleNone = 0,
    FakeRoleToplevel = 1,
    FakeRolePopup = 2
};

/** @brief Outcome a libwayland display call is told to produce. */
enum fake_mode_t {
    FakeOk = 0,
    FakeEagain, /**< Returns -1, errno EAGAIN, no display error. */
    FakeEintr, /**< Returns -1, errno EINTR, no display error. */
    FakeEio, /**< Returns -1, errno EIO, no display error. */
    FakeFault, /**< Returns -1, errno EIO, and wl_display_get_error becomes nonzero. */
    FakeFaultEagain /**< Returns -1, errno EAGAIN, and wl_display_get_error becomes nonzero. */
};

/** @brief Result the feature sender double returns for every frame. */
enum fake_send_mode_t {
    FakeSendAccept = 0, /**< HostFeatureSuccess; the frame is kept. */
    FakeSendFull, /**< HostFeatureQueueFull. */
    FakeSendDraining, /**< HostFeatureDraining. */
    FakeSendFail /**< HostFeatureNotReady, a result the handler must treat as fatal. */
};

/** @brief Kinds of queued compositor events. */
enum fake_event_kind_t {
    FakeEventAnnounce, /**< wl_registry.global: name, interface, version. */
    FakeEventRemove, /**< wl_registry.global_remove: name. */
    FakeEventPing, /**< xdg_wm_base.ping: serial. */
    FakeEventSurfaceConfigure, /**< xdg_surface.configure on `object`: serial. */
    FakeEventToplevelConfigure, /**< xdg_toplevel.configure on `object`: width, height, states. */
    FakeEventToplevelClose, /**< xdg_toplevel.close on `object`. */
    FakeEventDecorationConfigure, /**< zxdg_toplevel_decoration_v1.configure on `object`: mode. */
    FakeEventPopupConfigure, /**< xdg_popup.configure on `object`: x, y, width, height. */
    FakeEventPopupRepositioned, /**< xdg_popup.repositioned on `object`: token. */
    FakeEventPopupDone, /**< xdg_popup.popup_done on `object`. */
    FakeEventNoop /**< Makes the queue nonempty without a listener call. */
};

typedef struct fake_proxy_t {
    const char *iface; /* NULL for a slot never issued */
    uint8_t live;
    uint8_t role; /* enum fake_role_t, set on an xdg_surface */
    uint32_t version;
    const void *listener; /* implementation table, or NULL */
    void *data;
    unsigned aux; /* the object this one was built on (see fake_has_live_dependent); FakeNone when none */
    unsigned aux2; /* a popup's parent xdg_surface; FakeNone when none */
    uint32_t serials[FakeSerials]; /* configure serials sent to an xdg_surface and not yet acknowledged */
    uint8_t serial_count;
} fake_proxy_t;

typedef struct fake_request_t {
    unsigned proxy; /* FakeNone for a pointer the double never issued */
    const char *iface;
    uint32_t opcode;
    uint8_t destroy;
    long long a0, a1, a2, a3;
    char text[256];
} fake_request_t;

typedef struct fake_event_t {
    enum fake_event_kind_t kind;
    unsigned object; /* proxy index the event names; FakeNone for registry and wm_base events */
    uint32_t name; /* registry name, serial, token, mode or ping serial */
    uint32_t version;
    const char *iface;
    int32_t a, b, c, d; /* width and height, or popup x, y, width, height */
    uint32_t states[4];
    size_t state_count;
} fake_event_t;

static __attribute__((unused)) uintptr_t fake_storage[FakeProxies];
static __attribute__((unused)) fake_proxy_t fake_proxies[FakeProxies];
static __attribute__((unused)) unsigned fake_free[FakeProxies];
static __attribute__((unused)) unsigned fake_free_count;
static __attribute__((unused)) unsigned fake_high; /* slots ever used; the scan bound */
static __attribute__((unused)) fake_request_t fake_requests[FakeRequests];
static __attribute__((unused)) unsigned fake_request_count;
static __attribute__((unused)) const char *fake_log[FakeLog];
static __attribute__((unused)) unsigned fake_log_count;
static __attribute__((unused)) unsigned fake_grab_popup[FakeProxies];
static __attribute__((unused)) long long fake_grab_seat[FakeProxies];
static __attribute__((unused)) unsigned fake_grab_depth;
static __attribute__((unused)) unsigned fake_grants;
static __attribute__((unused)) fake_event_t fake_events[FakeEvents];
static __attribute__((unused)) unsigned fake_event_head;
static __attribute__((unused)) unsigned fake_event_tail;
static __attribute__((unused)) enum fake_mode_t fake_read_mode;
static __attribute__((unused)) enum fake_mode_t fake_flush_mode;
static __attribute__((unused)) enum fake_mode_t fake_roundtrip_mode;
static __attribute__((unused)) int fake_display_error;
static __attribute__((unused)) int fake_fd_value = FakeFd;
static __attribute__((unused)) unsigned fake_dispatch_calls;
static __attribute__((unused)) unsigned fake_dispatch_fail_at; /* the dispatch_pending call number that fails with EPROTO; 0 for none */
static __attribute__((unused)) unsigned fake_prepare_reads;
static __attribute__((unused)) unsigned fake_read_calls;
static __attribute__((unused)) unsigned fake_flushes;
static __attribute__((unused)) unsigned fake_roundtrips;
static __attribute__((unused)) unsigned fake_connects;
static __attribute__((unused)) unsigned fake_disconnects;
static __attribute__((unused)) int fake_connect_fail;
static __attribute__((unused)) const char *fake_fail_create; /* interface whose proxy creation returns NULL */
static __attribute__((unused)) unsigned fake_fail_create_skip; /* creations of that interface allowed before failing */
static __attribute__((unused)) const char *fake_fail_listener; /* interface whose listener registration fails */
static __attribute__((unused)) unsigned fake_creates, fake_destroys, fake_double_destroys;
static __attribute__((unused)) unsigned fake_order_violations; /* destroys that left a live dependent behind */
static __attribute__((unused)) unsigned fake_protocol_errors; /* see the file comment; reset by fake_begin */
static __attribute__((unused)) unsigned fake_dead_requests; /* requests on dead or unknown objects */
static __attribute__((unused)) unsigned fake_invalid_serials; /* acks of serials that were not outstanding */
static __attribute__((unused)) unsigned fake_invalid_grabs; /* grabs the compositor model refuses */
static __attribute__((unused)) unsigned fake_send_attempts;
static __attribute__((unused)) unsigned fake_send_accepted;
static __attribute__((unused)) unsigned fake_send_full;
static __attribute__((unused)) unsigned fake_send_draining;
static __attribute__((unused)) enum fake_send_mode_t fake_send_mode;
static __attribute__((unused)) int fake_sender_token; /* the sender the handler is given; any other sender is a protocol error */
static __attribute__((unused)) unsigned fake_sender_mismatch;
static __attribute__((unused)) uint8_t fake_frames[FakeFrames][FakeFrameBytes];
static __attribute__((unused)) size_t fake_frame_length[FakeFrames];
static __attribute__((unused)) unsigned fake_frame_count;

#define FakeSender ((interop_host_feature_sender_t *)(void *)&fake_sender_token)

/** @brief Proxy index of the object `pointer` names, or FakeNone for a foreign pointer. Dead slots are returned. */
static __attribute__((unused)) unsigned fake_index(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)fake_storage;
    if (address < base || address >= base + sizeof fake_storage) return FakeNone;
    if ((address - base) % sizeof fake_storage[0]) return FakeNone;
    return (unsigned)((address - base) / sizeof fake_storage[0]);
}

/** @brief Address of a proxy. */
static __attribute__((unused)) void *fake_object(unsigned index) {
    return (void *)&fake_storage[index];
}

/** @brief Whether an index names a slot that was issued at least once. */
static __attribute__((unused)) int fake_known(unsigned index) {
    return index != FakeNone && index < FakeProxies && fake_proxies[index].iface != NULL;
}

/** @brief Whether a pointer names a live proxy. */
static __attribute__((unused)) int fake_live_pointer(const void *pointer) {
    unsigned index = fake_index(pointer);
    return fake_known(index) && fake_proxies[index].live;
}

/** @brief Create a live proxy in a free slot (last freed first) or the next unused one. */
static __attribute__((unused)) unsigned fake_new(const char *iface, uint32_t version) {
    unsigned index;
    if (fake_free_count) {
        index = fake_free[--fake_free_count];
    } else {
        if (fake_high >= FakeProxies) __builtin_trap();
        index = fake_high++;
    }
    memset(&fake_proxies[index], 0, sizeof fake_proxies[index]);
    fake_proxies[index].iface = iface;
    fake_proxies[index].live = 1;
    fake_proxies[index].version = version;
    fake_proxies[index].aux = FakeNone;
    fake_proxies[index].aux2 = FakeNone;
    ++fake_creates;
    return index;
}

/** @brief True when a live proxy depends on `index` in a way that forbids destroying it now. */
static __attribute__((unused)) int fake_has_live_dependent(unsigned index) {
    const char *iface = fake_proxies[index].iface;
    for (unsigned other = 0; other < fake_high; ++other) {
        if (!fake_proxies[other].live || other == index) continue;
        const char *dependent = fake_proxies[other].iface;
        if (!dependent) continue;
        if (!strcmp(iface, "xdg_surface") && (!strcmp(dependent, "xdg_toplevel") || !strcmp(dependent, "xdg_popup")) && fake_proxies[other].aux == index) return 1;
        if (!strcmp(iface, "xdg_surface") && !strcmp(dependent, "xdg_popup") && fake_proxies[other].aux2 == index) return 1;
        if (!strcmp(iface, "wl_surface") && !strcmp(dependent, "xdg_surface") && fake_proxies[other].aux == index) return 1;
        if (!strcmp(iface, "xdg_toplevel") && !strcmp(dependent, "zxdg_toplevel_decoration_v1") && fake_proxies[other].aux == index) return 1;
        if (!strcmp(iface, "xdg_popup") && !strcmp(dependent, "xdg_popup") && fake_proxies[other].aux2 == fake_proxies[index].aux) return 1;
    }
    return 0;
}

/** @brief Stack position of a grabbing popup, or -1. */
static __attribute__((unused)) int fake_grab_at(unsigned popup) {
    for (unsigned at = 0; at < fake_grab_depth; ++at) {
        if (fake_grab_popup[at] == popup) return (int)at;
    }
    return -1;
}

/** @brief Topmost grabbing popup on `seat`, or FakeNone. */
static __attribute__((unused)) unsigned fake_seat_top(long long seat) {
    for (unsigned at = fake_grab_depth; at > 0; --at) {
        if (fake_grab_seat[at - 1] == seat) return fake_grab_popup[at - 1];
    }
    return FakeNone;
}

/** @brief Live popup whose own xdg_surface is `surface`, or FakeNone. */
static __attribute__((unused)) unsigned fake_popup_of_surface(unsigned surface) {
    for (unsigned index = 0; index < fake_high; ++index) {
        if (fake_proxies[index].live && fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, "xdg_popup") && fake_proxies[index].aux == surface) return index;
    }
    return FakeNone;
}

/**
 * @brief Compositor rule for xdg_popup.grab: a popup on a toplevel grabs only while its seat has no active grab; a popup
 * on a popup grabs only when its parent is the topmost grabbing popup of that seat.
 */
static __attribute__((unused)) int fake_grab_allowed(unsigned popup, long long seat) {
    unsigned parent = fake_proxies[popup].aux2;
    if (parent == FakeNone || fake_grab_at(popup) >= 0) return 0;
    if (fake_proxies[parent].role == FakeRoleToplevel) return fake_seat_top(seat) == FakeNone;
    if (fake_proxies[parent].role == FakeRolePopup) {
        unsigned parent_popup = fake_popup_of_surface(parent);
        return parent_popup != FakeNone && fake_seat_top(seat) == parent_popup;
    }
    return 0;
}

/** @brief Remove a grabbing popup and every grab stacked above it on the same seat. */
static __attribute__((unused)) void fake_grab_pop(unsigned popup) {
    int at = fake_grab_at(popup);
    if (at < 0) return;
    long long seat = fake_grab_seat[at];
    unsigned out = (unsigned)at;
    for (unsigned from = (unsigned)at; from < fake_grab_depth; ++from) {
        if (fake_grab_seat[from] == seat) continue;
        fake_grab_popup[out] = fake_grab_popup[from];
        fake_grab_seat[out] = fake_grab_seat[from];
        ++out;
    }
    fake_grab_depth = out;
}

/** @brief Destroy a proxy: log it, free its slot. A destroy of a dead proxy is a double destroy. */
static __attribute__((unused)) void fake_destroy_index(unsigned index) {
    if (index == FakeNone || !fake_known(index)) {
        ++fake_protocol_errors;
        return;
    }
    if (!fake_proxies[index].live) {
        ++fake_double_destroys;
        return;
    }
    if (fake_has_live_dependent(index)) ++fake_order_violations;
    if (!strcmp(fake_proxies[index].iface, "xdg_popup")) fake_grab_pop(index);
    if (fake_log_count >= FakeLog) __builtin_trap();
    fake_log[fake_log_count++] = fake_proxies[index].iface;
    fake_proxies[index].live = 0;
    fake_free[fake_free_count++] = index;
    ++fake_destroys;
}

/** @brief Index of the first live proxy with `iface`, or FakeNone. */
static __attribute__((unused)) unsigned fake_live_first(const char *iface) {
    for (unsigned index = 0; index < fake_high; ++index) {
        if (fake_proxies[index].live && !strcmp(fake_proxies[index].iface, iface)) return index;
    }
    return FakeNone;
}

/** @brief Number of live proxies, the display included. */
static __attribute__((unused)) unsigned fake_live_count(void) {
    unsigned count = 0;
    for (unsigned index = 0; index < fake_high; ++index) {
        if (fake_proxies[index].live) ++count;
    }
    return count;
}

/** @brief Arguments decoded per interface and opcode. A constructor's NULL new-id placeholder is skipped. */
static __attribute__((unused)) void decode_args(fake_request_t *request, const char *iface, uint32_t opcode, va_list *ap) {
#define ArgPtr(slot) do { void *p = va_arg(*ap, void *); request->a##slot = p ? (long long)fake_index(p) : -1; } while (0)
#define ArgInt(slot) request->a##slot = (long long)va_arg(*ap, int)
#define ArgUint(slot) request->a##slot = (long long)va_arg(*ap, uint32_t)
#define ArgText() do { const char *t = va_arg(*ap, const char *); snprintf(request->text, sizeof request->text, "%s", t ? t : "(null)"); } while (0)
#define SkipNewId() ((void)va_arg(*ap, void *))
    if (!strcmp(iface, "wl_display")) {
        if (opcode == WL_DISPLAY_GET_REGISTRY) SkipNewId();
    } else if (!strcmp(iface, "wl_registry")) {
        if (opcode == WL_REGISTRY_BIND) {
            ArgUint(0);
            ArgText();
            ArgUint(1);
        }
    } else if (!strcmp(iface, "wl_compositor")) {
        if (opcode == WL_COMPOSITOR_CREATE_SURFACE) SkipNewId();
    } else if (!strcmp(iface, "xdg_wm_base")) {
        if (opcode == XDG_WM_BASE_GET_XDG_SURFACE) { SkipNewId(); ArgPtr(0); }
        else if (opcode == XDG_WM_BASE_CREATE_POSITIONER) SkipNewId();
        else if (opcode == XDG_WM_BASE_PONG) ArgUint(0);
    } else if (!strcmp(iface, "xdg_surface")) {
        if (opcode == XDG_SURFACE_GET_TOPLEVEL) SkipNewId();
        else if (opcode == XDG_SURFACE_GET_POPUP) { SkipNewId(); ArgPtr(0); ArgPtr(1); }
        else if (opcode == XDG_SURFACE_ACK_CONFIGURE) ArgUint(0);
    } else if (!strcmp(iface, "xdg_toplevel")) {
        if (opcode == XDG_TOPLEVEL_SET_PARENT) ArgPtr(0);
        else if (opcode == XDG_TOPLEVEL_SET_TITLE || opcode == XDG_TOPLEVEL_SET_APP_ID) ArgText();
        else if (opcode == XDG_TOPLEVEL_SET_MIN_SIZE) { ArgInt(0); ArgInt(1); }
        else if (opcode == XDG_TOPLEVEL_MOVE) { ArgPtr(0); ArgUint(1); }
        else if (opcode == XDG_TOPLEVEL_RESIZE) { ArgPtr(0); ArgUint(1); ArgUint(2); }
    } else if (!strcmp(iface, "xdg_positioner")) {
        if (opcode == XDG_POSITIONER_SET_SIZE || opcode == XDG_POSITIONER_SET_OFFSET) { ArgInt(0); ArgInt(1); }
        else if (opcode == XDG_POSITIONER_SET_ANCHOR_RECT) { ArgInt(0); ArgInt(1); ArgInt(2); ArgInt(3); }
        else if (opcode == XDG_POSITIONER_SET_ANCHOR || opcode == XDG_POSITIONER_SET_GRAVITY || opcode == XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT) ArgUint(0);
    } else if (!strcmp(iface, "xdg_popup")) {
        if (opcode == XDG_POPUP_GRAB) {
            /* A seat the double issued is keyed by its index; any other seat by its pointer value. */
            void *seat = va_arg(*ap, void *);
            unsigned index = seat ? fake_index(seat) : FakeNone;
            request->a0 = !seat ? -1 : fake_known(index) ? (long long)index : (long long)(uintptr_t)seat;
            ArgUint(1);
        } else if (opcode == XDG_POPUP_REPOSITION) { ArgPtr(0); ArgUint(1); }
    } else if (!strcmp(iface, "zxdg_decoration_manager_v1")) {
        if (opcode == ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) { SkipNewId(); ArgPtr(0); }
    } else if (!strcmp(iface, "zxdg_toplevel_decoration_v1")) {
        if (opcode == ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE) ArgUint(0);
    }
#undef ArgPtr
#undef ArgInt
#undef ArgUint
#undef ArgText
#undef SkipNewId
}

/** @brief Record a configure serial sent to an xdg_surface; the oldest is dropped beyond FakeSerials. */
static __attribute__((unused)) void fake_add_serial(unsigned index, uint32_t serial) {
    fake_proxy_t *proxy = &fake_proxies[index];
    if (proxy->serial_count == FakeSerials) {
        memmove(proxy->serials, proxy->serials + 1, sizeof proxy->serials[0] * (FakeSerials - 1));
        proxy->serial_count--;
    }
    proxy->serials[proxy->serial_count++] = serial;
}

/** @brief Consume an outstanding configure serial. Returns 0 when the serial was never sent or already acknowledged. */
static __attribute__((unused)) int fake_take_serial(unsigned index, uint32_t serial) {
    fake_proxy_t *proxy = &fake_proxies[index];
    for (unsigned at = 0; at < proxy->serial_count; ++at) {
        if (proxy->serials[at] != serial) continue;
        memmove(proxy->serials + at, proxy->serials + at + 1, sizeof proxy->serials[0] * (proxy->serial_count - at - 1));
        proxy->serial_count--;
        return 1;
    }
    return 0;
}

/** @brief Role an xdg_surface's constructor assigns. A second role is a protocol error. */
static __attribute__((unused)) void fake_assign_role(unsigned surface, uint8_t role) {
    if (surface == FakeNone) return;
    if (fake_proxies[surface].role != FakeRoleNone) ++fake_protocol_errors;
    fake_proxies[surface].role = role;
}

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    unsigned index = fake_index(proxy);
    int destroying = (flags & WL_MARSHAL_FLAG_DESTROY) != 0;
    int known = fake_known(index);
    int dead = !known || !fake_proxies[index].live;
    if (dead && !destroying) {
        ++fake_protocol_errors;
        ++fake_dead_requests;
    }
    const char *iface = known ? fake_proxies[index].iface : NULL;
    if (fake_request_count >= FakeRequests) __builtin_trap();
    fake_request_t *request = &fake_requests[fake_request_count++];
    memset(request, 0, sizeof *request);
    request->proxy = known ? index : FakeNone;
    request->iface = iface;
    request->opcode = opcode;
    request->destroy = (uint8_t)destroying;
    va_list ap;
    va_start(ap, flags);
    if (iface && !dead) decode_args(request, iface, opcode, &ap);
    va_end(ap);
    if (!dead && iface && !strcmp(iface, "xdg_surface") && opcode == XDG_SURFACE_ACK_CONFIGURE && !fake_take_serial(index, (uint32_t)request->a0)) {
        ++fake_protocol_errors;
        ++fake_invalid_serials;
    }
    if (!dead && iface && !strcmp(iface, "xdg_popup") && opcode == XDG_POPUP_GRAB) {
        if (fake_grab_allowed(index, request->a0)) {
            fake_grab_popup[fake_grab_depth] = index;
            fake_grab_seat[fake_grab_depth] = request->a0;
            ++fake_grab_depth;
            ++fake_grants;
        } else {
            ++fake_protocol_errors;
            ++fake_invalid_grabs;
        }
    }
    if (destroying) fake_destroy_index(index);
    if (interface == NULL) return NULL;
    if (fake_fail_create && !strcmp(interface->name, fake_fail_create)) {
        if (fake_fail_create_skip == 0) return NULL;
        --fake_fail_create_skip;
    }
    unsigned created = fake_new(interface->name, version);
    if (iface && !strcmp(iface, "xdg_wm_base") && opcode == XDG_WM_BASE_GET_XDG_SURFACE) fake_proxies[created].aux = (unsigned)request->a0;
    if (iface && !strcmp(iface, "xdg_surface") && opcode == XDG_SURFACE_GET_TOPLEVEL) {
        fake_proxies[created].aux = index;
        fake_assign_role(index, FakeRoleToplevel);
    }
    if (iface && !strcmp(iface, "xdg_surface") && opcode == XDG_SURFACE_GET_POPUP) {
        fake_proxies[created].aux = index;
        fake_proxies[created].aux2 = (unsigned)request->a0;
        fake_assign_role(index, FakeRolePopup);
    }
    if (iface && !strcmp(iface, "zxdg_decoration_manager_v1")) fake_proxies[created].aux = (unsigned)request->a0;
    return (struct wl_proxy *)fake_object(created);
}

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    unsigned index = fake_index(proxy);
    return fake_known(index) ? fake_proxies[index].version : 0u;
}

int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = fake_index(proxy);
    if (!fake_known(index) || !fake_proxies[index].live) {
        ++fake_protocol_errors;
        ++fake_dead_requests;
        return -1;
    }
    if (fake_fail_listener && !strcmp(fake_proxies[index].iface, fake_fail_listener)) return -1;
    if (fake_proxies[index].listener != NULL) return -1;
    fake_proxies[index].listener = (const void *)implementation;
    fake_proxies[index].data = data;
    return 0;
}

void __wrap_wl_proxy_destroy(struct wl_proxy *proxy) {
    fake_destroy_index(fake_index(proxy));
}

/* ---- compositor events ---------------------------------------------------------------------------- */

static __attribute__((unused)) void fake_queue(const fake_event_t *event) {
    if (fake_event_tail >= FakeEvents) __builtin_trap();
    fake_events[fake_event_tail++] = *event;
}

static __attribute__((unused)) fake_event_t fake_event_blank(enum fake_event_kind_t kind, unsigned object) {
    fake_event_t event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.object = object;
    return event;
}

/** @brief Queue wl_registry.global for the next dispatch or roundtrip. */
static __attribute__((unused)) void fake_queue_announce(uint32_t name, const char *iface, uint32_t version) {
    fake_event_t event = fake_event_blank(FakeEventAnnounce, FakeNone);
    event.name = name;
    event.iface = iface;
    event.version = version;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_remove(uint32_t name) {
    fake_event_t event = fake_event_blank(FakeEventRemove, FakeNone);
    event.name = name;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_ping(uint32_t serial) {
    fake_event_t event = fake_event_blank(FakeEventPing, FakeNone);
    event.name = serial;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_noop(void) {
    fake_queue(&(fake_event_t){.kind = FakeEventNoop, .object = FakeNone});
}

static __attribute__((unused)) void fake_queue_surface_configure(unsigned surface, uint32_t serial) {
    fake_event_t event = fake_event_blank(FakeEventSurfaceConfigure, surface);
    event.name = serial;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_toplevel_configure(unsigned toplevel, int32_t width, int32_t height, const uint32_t *states, size_t count) {
    fake_event_t event = fake_event_blank(FakeEventToplevelConfigure, toplevel);
    event.a = width;
    event.b = height;
    if (count > 4) count = 4;
    if (count) memcpy(event.states, states, sizeof event.states[0] * count);
    event.state_count = count;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_toplevel_close(unsigned toplevel) {
    fake_queue(&(fake_event_t){.kind = FakeEventToplevelClose, .object = toplevel});
}

static __attribute__((unused)) void fake_queue_decoration_configure(unsigned decoration, uint32_t mode) {
    fake_event_t event = fake_event_blank(FakeEventDecorationConfigure, decoration);
    event.name = mode;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_popup_configure(unsigned popup, int32_t x, int32_t y, int32_t width, int32_t height) {
    fake_event_t event = fake_event_blank(FakeEventPopupConfigure, popup);
    event.a = x;
    event.b = y;
    event.c = width;
    event.d = height;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_popup_repositioned(unsigned popup, uint32_t token) {
    fake_event_t event = fake_event_blank(FakeEventPopupRepositioned, popup);
    event.name = token;
    fake_queue(&event);
}

static __attribute__((unused)) void fake_queue_popup_done(unsigned popup) {
    fake_queue(&(fake_event_t){.kind = FakeEventPopupDone, .object = popup});
}

/** @brief Deliver one event through its listener. Object events are dropped when the object is dead or unlistened. */
static __attribute__((unused)) void fake_deliver(const fake_event_t *event) {
    unsigned index;
    switch (event->kind) {
    case FakeEventAnnounce:
        index = fake_live_first("wl_registry");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct wl_registry_listener *)fake_proxies[index].listener)->global(fake_proxies[index].data, (struct wl_registry *)fake_object(index), event->name, event->iface, event->version);
        return;
    case FakeEventRemove:
        index = fake_live_first("wl_registry");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct wl_registry_listener *)fake_proxies[index].listener)->global_remove(fake_proxies[index].data, (struct wl_registry *)fake_object(index), event->name);
        return;
    case FakeEventPing:
        index = fake_live_first("xdg_wm_base");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct xdg_wm_base_listener *)fake_proxies[index].listener)->ping(fake_proxies[index].data, (struct xdg_wm_base *)fake_object(index), event->name);
        return;
    case FakeEventNoop:
        return;
    default:
        break;
    }
    index = event->object;
    if (!fake_known(index) || !fake_proxies[index].live || fake_proxies[index].listener == NULL) return;
    const void *listener = fake_proxies[index].listener;
    void *data = fake_proxies[index].data;
    switch (event->kind) {
    case FakeEventSurfaceConfigure:
        fake_add_serial(index, event->name);
        ((const struct xdg_surface_listener *)listener)->configure(data, (struct xdg_surface *)fake_object(index), event->name);
        return;
    case FakeEventToplevelConfigure: {
        struct wl_array array;
        array.size = event->state_count * sizeof event->states[0];
        array.alloc = array.size;
        array.data = (void *)event->states;
        ((const struct xdg_toplevel_listener *)listener)->configure(data, (struct xdg_toplevel *)fake_object(index), event->a, event->b, &array);
        return;
    }
    case FakeEventToplevelClose:
        ((const struct xdg_toplevel_listener *)listener)->close(data, (struct xdg_toplevel *)fake_object(index));
        return;
    case FakeEventDecorationConfigure:
        ((const struct zxdg_toplevel_decoration_v1_listener *)listener)->configure(data, (struct zxdg_toplevel_decoration_v1 *)fake_object(index), event->name);
        return;
    case FakeEventPopupConfigure:
        ((const struct xdg_popup_listener *)listener)->configure(data, (struct xdg_popup *)fake_object(index), event->a, event->b, event->c, event->d);
        return;
    case FakeEventPopupRepositioned:
        ((const struct xdg_popup_listener *)listener)->repositioned(data, (struct xdg_popup *)fake_object(index), event->name);
        return;
    case FakeEventPopupDone:
        fake_grab_pop(index);
        ((const struct xdg_popup_listener *)listener)->popup_done(data, (struct xdg_popup *)fake_object(index));
        return;
    default:
        return;
    }
}

/** @brief Deliver every queued event in order. */
static __attribute__((unused)) unsigned fake_deliver_queued(void) {
    unsigned delivered = 0;
    while (fake_event_head < fake_event_tail) {
        fake_event_t event = fake_events[fake_event_head++];
        fake_deliver(&event);
        ++delivered;
    }
    /* Drained: the indices start again, so FakeEvents bounds the queue, not the run. */
    fake_event_head = 0;
    fake_event_tail = 0;
    return delivered;
}

/** @brief Apply a mode to a call that returns int. */
static __attribute__((unused)) int fake_fail_with(enum fake_mode_t mode) {
    switch (mode) {
    case FakeOk:
        return 0;
    case FakeEagain:
        errno = EAGAIN;
        return -1;
    case FakeEintr:
        errno = EINTR;
        return -1;
    case FakeEio:
        errno = EIO;
        return -1;
    case FakeFault:
        fake_display_error = EIO;
        errno = EIO;
        return -1;
    case FakeFaultEagain:
        fake_display_error = EPROTO;
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

/* ---- libwayland display entry points ------------------------------------------------------------- */

struct wl_display *__wrap_wl_display_connect(const char *name) {
    (void)name;
    ++fake_connects;
    if (fake_connect_fail) return NULL;
    return (struct wl_display *)fake_object(fake_new("wl_display", 1));
}

void __wrap_wl_display_disconnect(struct wl_display *display) {
    unsigned index = fake_index(display);
    ++fake_disconnects;
    fake_destroy_index(index);
}

int __wrap_wl_display_get_fd(struct wl_display *display) {
    (void)display;
    return fake_fd_value;
}

int __wrap_wl_display_get_error(struct wl_display *display) {
    (void)display;
    return fake_display_error;
}

int __wrap_wl_display_dispatch_pending(struct wl_display *display) {
    (void)display;
    ++fake_dispatch_calls;
    if (fake_dispatch_fail_at && fake_dispatch_calls == fake_dispatch_fail_at) {
        errno = EPROTO;
        return -1;
    }
    return (int)fake_deliver_queued();
}

int __wrap_wl_display_prepare_read(struct wl_display *display) {
    (void)display;
    ++fake_prepare_reads;
    if (fake_event_tail > fake_event_head) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int __wrap_wl_display_read_events(struct wl_display *display) {
    (void)display;
    ++fake_read_calls;
    return fake_fail_with(fake_read_mode);
}

int __wrap_wl_display_flush(struct wl_display *display) {
    (void)display;
    ++fake_flushes;
    return fake_fail_with(fake_flush_mode);
}

int __wrap_wl_display_roundtrip(struct wl_display *display) {
    (void)display;
    ++fake_roundtrips;
    int result = fake_fail_with(fake_roundtrip_mode);
    if (result == 0) fake_deliver_queued();
    return result;
}

/* ---- feature sender -------------------------------------------------------------------------------- */

int __wrap_interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length) {
    ++fake_send_attempts;
    if ((void *)sender != (void *)&fake_sender_token) ++fake_sender_mismatch;
    switch (fake_send_mode) {
    case FakeSendAccept:
        if (length > FakeFrameBytes || fake_frame_count >= FakeFrames) __builtin_trap();
        memcpy(fake_frames[fake_frame_count], frame, length);
        fake_frame_length[fake_frame_count] = length;
        ++fake_frame_count;
        ++fake_send_accepted;
        return HostFeatureSuccess;
    case FakeSendFull:
        ++fake_send_full;
        return HostFeatureQueueFull;
    case FakeSendDraining:
        ++fake_send_draining;
        return HostFeatureDraining;
    case FakeSendFail:
        return HostFeatureNotReady;
    }
    return HostFeatureNotReady;
}

/* ---- recording state ------------------------------------------------------------------------------- */

/** @brief Reset every recording table. The protocol error counters are reset too; the test entry point checks them. */
static __attribute__((unused)) void fake_begin(void) {
    memset(fake_storage, 0, sizeof fake_storage);
    memset(fake_proxies, 0, sizeof fake_proxies);
    memset(fake_requests, 0, sizeof fake_requests);
    memset(fake_events, 0, sizeof fake_events);
    memset(fake_frame_length, 0, sizeof fake_frame_length);
    fake_free_count = 0;
    fake_high = 0;
    fake_request_count = 0;
    fake_log_count = 0;
    fake_grab_depth = 0;
    fake_grants = 0;
    fake_event_head = 0;
    fake_event_tail = 0;
    fake_read_mode = FakeOk;
    fake_flush_mode = FakeOk;
    fake_roundtrip_mode = FakeOk;
    fake_display_error = 0;
    fake_fd_value = FakeFd;
    fake_dispatch_calls = 0;
    fake_dispatch_fail_at = 0;
    fake_prepare_reads = 0;
    fake_read_calls = 0;
    fake_flushes = 0;
    fake_roundtrips = 0;
    fake_connects = 0;
    fake_disconnects = 0;
    fake_connect_fail = 0;
    fake_fail_create = NULL;
    fake_fail_create_skip = 0;
    fake_fail_listener = NULL;
    fake_creates = fake_destroys = fake_double_destroys = 0;
    fake_order_violations = 0;
    fake_protocol_errors = 0;
    fake_dead_requests = 0;
    fake_invalid_serials = 0;
    fake_invalid_grabs = 0;
    fake_send_attempts = 0;
    fake_send_accepted = 0;
    fake_send_full = 0;
    fake_send_draining = 0;
    fake_send_mode = FakeSendAccept;
    fake_sender_mismatch = 0;
    fake_frame_count = 0;
}

/** @brief Forget the recorded requests, the destroy log and the accepted frames, keeping every proxy and counter. */
static __attribute__((unused)) void fake_clear_recording(void) {
    fake_request_count = 0;
    fake_log_count = 0;
    fake_frame_count = 0;
}

/** @brief Number of recorded requests with `iface` and `opcode`. */
static __attribute__((unused)) unsigned fake_count_requests(const char *iface, uint32_t opcode) {
    unsigned count = 0;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        const fake_request_t *request = &fake_requests[index];
        if (request->iface && !strcmp(request->iface, iface) && request->opcode == opcode) ++count;
    }
    return count;
}

/** @brief Index of the first recorded request of `iface`/`opcode` at or after `from`, or -1. */
static __attribute__((unused)) int fake_find(const char *iface, uint32_t opcode, unsigned from) {
    for (unsigned index = from; index < fake_request_count; ++index) {
        if (fake_requests[index].iface && !strcmp(fake_requests[index].iface, iface) && fake_requests[index].opcode == opcode) return (int)index;
    }
    return -1;
}

/** @brief Render the requests recorded from `from` on as "interface:opcode" words, with "!" for a destroy. */
static __attribute__((unused)) void fake_signature(char *out, size_t capacity, unsigned from) {
    size_t used = 0;
    out[0] = '\0';
    for (unsigned index = from; index < fake_request_count; ++index) {
        if (!fake_requests[index].iface) continue;
        int written = snprintf(out + used, capacity - used, "%s%s:%u%s", used ? " " : "", fake_requests[index].iface, fake_requests[index].opcode, fake_requests[index].destroy ? "!" : "");
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
}

#endif
