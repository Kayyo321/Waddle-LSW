/**
 * @file host_wayland_windows_fake.h
 * @brief Recording Wayland double for host_wayland_windows tests.
 *
 * Included by a test translation unit after the module source. Every created object gets a record index
 * that only grows, so `fake_nth` keeps creation order. Its storage address is a slot from a pool, and a
 * destroyed object's slot is handed to the next creation (last in, first out). A stale pointer therefore
 * can name a live object, as with a real allocator; the module must not rely on pointer identity alone.
 *
 * Events are dispatched only to live objects, as libwayland does after wl_proxy_destroy. A late event for a
 * destroyed object is therefore dropped by the double; the module's own generation check is tested directly.
 *
 * The double is strict about what a compositor would reject, and each rejection increments
 * `fake_protocol_errors`, which the test entry point requires to stay zero:
 *  - a request on a destroyed object, or on a pointer the double never issued;
 *  - an xdg_surface.ack_configure whose serial was not sent to that surface and not yet acknowledged;
 *  - a second role on one xdg_surface;
 *  - a listener installed twice (refused with -1, as libwayland does).
 */
#ifndef HostWaylandWindowsFakeH
#define HostWaylandWindowsFakeH

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAKE_PROXIES 512u
#define FAKE_ADDRESSES 512u
#define FAKE_REQUESTS 4096u
#define FAKE_SERIALS 8u
#define FAKE_NONE 0xFFFFFFFFu

/** @brief Role an xdg_surface received; an xdg_surface takes at most one. */
enum fake_role_t {
    FAKE_ROLE_NONE = 0,
    FAKE_ROLE_TOPLEVEL = 1,
    FAKE_ROLE_POPUP = 2
};

typedef struct fake_proxy_t {
    const char *iface; /* NULL for an unused slot */
    uint8_t live;
    uint8_t role; /* enum fake_role_t, set on an xdg_surface */
    const void *listener;
    void *data;
    unsigned address; /* storage slot given at creation; freed on destroy and reused later */
    /* Dependencies for the destroy-order invariants. aux is the object this one was built on (an xdg_surface's
     * wl_surface, a toplevel's or popup's xdg_surface, a decoration's toplevel); aux2 is a popup's parent
     * xdg_surface. FAKE_NONE when not applicable. */
    unsigned aux;
    unsigned aux2;
    /* Configure serials sent to an xdg_surface and not yet acknowledged. */
    uint32_t serials[FAKE_SERIALS];
    uint8_t serial_count;
} fake_proxy_t;

typedef struct fake_request_t {
    unsigned proxy; /* FAKE_NONE for a proxy the fake does not own */
    const char *iface;
    uint32_t opcode;
    uint8_t destroy;
    long long a0, a1, a2, a3;
    char text[1100];
} fake_request_t;

static uintptr_t fake_storage[FAKE_ADDRESSES];
static unsigned fake_address_owner[FAKE_ADDRESSES]; /* proxy index last given the slot; FAKE_NONE when never used */
static unsigned fake_free_slots[FAKE_ADDRESSES];
static unsigned fake_free_count;
static unsigned fake_next_slot;
static fake_proxy_t fake_proxies[FAKE_PROXIES];
static unsigned fake_proxy_count;
static unsigned fake_creates, fake_destroys, fake_double_destroys;
static unsigned fake_order_violations; /* destroys that left a live dependent behind */
static unsigned fake_protocol_errors; /* every rejection listed in the file comment; never reset */
static unsigned fake_dead_requests; /* requests on destroyed or unknown objects */
static unsigned fake_invalid_serials; /* acks of serials that were not outstanding */
static unsigned fake_invalid_grabs; /* grabs the compositor model refuses with invalid_grab; never reset */
static unsigned fake_grants; /* grabs the compositor model accepted since the last fake_begin */
/* Grabbing popups in grant order. A seat's grab stack is the entries carrying that seat, last one on top. */
static unsigned fake_grab_popup[FAKE_PROXIES];
static long long fake_grab_seat[FAKE_PROXIES];
static unsigned fake_grab_depth;
static fake_request_t fake_requests[FAKE_REQUESTS];
static unsigned fake_request_count;
static const char *fake_fail_create; /* interface whose creation returns NULL */
static unsigned fake_fail_create_skip; /* creations of that interface allowed before failing */
static const char *fake_fail_listener; /* interface whose listener registration fails */
static uint32_t fake_version = 5;

/** @brief Proxy index the storage slot of `pointer` was last given to, or FAKE_NONE for a foreign pointer. */
static unsigned fake_index(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)fake_storage;
    if (address < base || address >= base + sizeof fake_storage || (address - base) % sizeof fake_storage[0]) return FAKE_NONE;
    return fake_address_owner[(address - base) / sizeof fake_storage[0]];
}

static void *fake_object(unsigned index) {
    return (void *)&fake_storage[fake_proxies[index].address];
}

static unsigned fake_new(const char *iface) {
    if (fake_proxy_count >= FAKE_PROXIES) __builtin_trap();
    unsigned slot;
    if (fake_free_count) {
        slot = fake_free_slots[--fake_free_count];
    } else {
        if (fake_next_slot >= FAKE_ADDRESSES) __builtin_trap();
        slot = fake_next_slot++;
    }
    unsigned index = fake_proxy_count++;
    memset(&fake_proxies[index], 0, sizeof fake_proxies[index]);
    fake_proxies[index].iface = iface;
    fake_proxies[index].live = 1;
    fake_proxies[index].address = slot;
    fake_proxies[index].aux = FAKE_NONE;
    fake_proxies[index].aux2 = FAKE_NONE;
    fake_address_owner[slot] = index;
    ++fake_creates;
    return index;
}

/** @brief True when a live proxy depends on `index` in a way that forbids destroying it now. */
static int fake_has_live_dependent(unsigned index) {
    const char *iface = fake_proxies[index].iface;
    for (unsigned other = 0; other < fake_proxy_count; ++other) {
        if (!fake_proxies[other].live || other == index) continue;
        const char *dependent = fake_proxies[other].iface;
        if (!dependent) continue;
        if (!strcmp(iface, "xdg_surface") && (!strcmp(dependent, "xdg_toplevel") || !strcmp(dependent, "xdg_popup")) && fake_proxies[other].aux == index) return 1;
        if (!strcmp(iface, "xdg_surface") && !strcmp(dependent, "xdg_popup") && fake_proxies[other].aux2 == index) return 1;
        if (!strcmp(iface, "wl_surface") && !strcmp(dependent, "xdg_surface") && fake_proxies[other].aux == index) return 1;
        if (!strcmp(iface, "xdg_toplevel") && !strcmp(dependent, "zxdg_toplevel_decoration_v1") && fake_proxies[other].aux == index) return 1;
        /* A child popup is built on its parent's xdg_surface, so the parent popup waits for it. */
        if (!strcmp(iface, "xdg_popup") && !strcmp(dependent, "xdg_popup") && fake_proxies[other].aux2 == fake_proxies[index].aux) return 1;
    }
    return 0;
}

/** @brief Stack position of a grabbing popup, or -1. */
static int fake_grab_at(unsigned popup) {
    for (unsigned at = 0; at < fake_grab_depth; ++at) {
        if (fake_grab_popup[at] == popup) return (int)at;
    }
    return -1;
}

/** @brief Topmost grabbing popup on `seat`, or FAKE_NONE. */
static unsigned fake_seat_top(long long seat) {
    for (unsigned at = fake_grab_depth; at > 0; --at) {
        if (fake_grab_seat[at - 1] == seat) return fake_grab_popup[at - 1];
    }
    return FAKE_NONE;
}

/** @brief Popup whose own xdg_surface is `surface`, or FAKE_NONE. Only a live popup counts. */
static unsigned fake_popup_of_surface(unsigned surface) {
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].live && fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, "xdg_popup") && fake_proxies[index].aux == surface) return index;
    }
    return FAKE_NONE;
}

/**
 * @brief Compositor rule for xdg_popup.grab: a popup on a toplevel may grab only when the seat has no active
 * grab; a popup on a popup may grab only when its parent is the topmost grabbing popup of that seat. Anything
 * else is invalid_grab.
 */
static int fake_grab_allowed(unsigned popup, long long seat) {
    unsigned parent = fake_proxies[popup].aux2;
    if (parent == FAKE_NONE || fake_grab_at(popup) >= 0) return 0;
    if (fake_proxies[parent].role == FAKE_ROLE_TOPLEVEL) return fake_seat_top(seat) == FAKE_NONE;
    if (fake_proxies[parent].role == FAKE_ROLE_POPUP) {
        unsigned parent_popup = fake_popup_of_surface(parent);
        return parent_popup != FAKE_NONE && fake_seat_top(seat) == parent_popup;
    }
    return 0;
}

/** @brief Remove a grabbing popup and every grab stacked above it on the same seat (dismiss or destroy). */
static void fake_grab_pop(unsigned popup) {
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

static void fake_destroy_index(unsigned index) {
    if (index == FAKE_NONE) return;
    if (!fake_proxies[index].live) { ++fake_double_destroys; return; }
    if (fake_has_live_dependent(index)) ++fake_order_violations;
    if (fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, "xdg_popup")) fake_grab_pop(index);
    fake_proxies[index].live = 0;
    fake_free_slots[fake_free_count++] = fake_proxies[index].address;
    ++fake_destroys;
}

/** @brief Reset the recording state. Protocol-error counters are not reset: they are checked once, at the end. */
static void fake_begin(void) {
    memset(fake_storage, 0, sizeof fake_storage);
    memset(fake_address_owner, 0xFF, sizeof fake_address_owner);
    memset(fake_proxies, 0, sizeof fake_proxies);
    memset(fake_requests, 0, sizeof fake_requests);
    fake_proxy_count = 0;
    fake_free_count = 0;
    fake_next_slot = 0;
    fake_creates = fake_destroys = fake_double_destroys = 0;
    fake_request_count = 0;
    fake_fail_create = NULL;
    fake_fail_create_skip = 0;
    fake_fail_listener = NULL;
    fake_version = 5;
    fake_grants = 0;
    fake_grab_depth = 0;
}

/* Arguments are decoded per interface and opcode into the recorded slots. A constructor request passes a
 * NULL placeholder for its new-id argument before the real arguments; that placeholder is skipped here. */
static void decode_args(fake_request_t *request, const char *iface, uint32_t opcode, va_list *ap) {
#define ARG_PTR(slot) do { void *p = va_arg(*ap, void *); request->a##slot = p ? (long long)fake_index(p) : -1; } while (0)
#define ARG_INT(slot) request->a##slot = (long long)va_arg(*ap, int)
#define ARG_UINT(slot) request->a##slot = (long long)va_arg(*ap, uint32_t)
#define ARG_TEXT() do { const char *t = va_arg(*ap, const char *); snprintf(request->text, sizeof request->text, "%s", t ? t : "(null)"); } while (0)
#define SKIP_NEW_ID() ((void)va_arg(*ap, void *))
    if (!strcmp(iface, "xdg_wm_base")) {
        if (opcode == XDG_WM_BASE_GET_XDG_SURFACE) { SKIP_NEW_ID(); ARG_PTR(0); }
    } else if (!strcmp(iface, "xdg_surface")) {
        if (opcode == XDG_SURFACE_GET_TOPLEVEL) { SKIP_NEW_ID(); }
        else if (opcode == XDG_SURFACE_GET_POPUP) { SKIP_NEW_ID(); ARG_PTR(0); ARG_PTR(1); }
        else if (opcode == XDG_SURFACE_ACK_CONFIGURE) ARG_UINT(0);
    } else if (!strcmp(iface, "xdg_toplevel")) {
        if (opcode == XDG_TOPLEVEL_SET_PARENT) ARG_PTR(0);
        else if (opcode == XDG_TOPLEVEL_SET_TITLE || opcode == XDG_TOPLEVEL_SET_APP_ID) ARG_TEXT();
        else if (opcode == XDG_TOPLEVEL_SET_MIN_SIZE) { ARG_INT(0); ARG_INT(1); }
    } else if (!strcmp(iface, "xdg_positioner")) {
        if (opcode == XDG_POSITIONER_SET_SIZE || opcode == XDG_POSITIONER_SET_OFFSET) { ARG_INT(0); ARG_INT(1); }
        else if (opcode == XDG_POSITIONER_SET_ANCHOR_RECT) { ARG_INT(0); ARG_INT(1); ARG_INT(2); ARG_INT(3); }
        else if (opcode == XDG_POSITIONER_SET_ANCHOR || opcode == XDG_POSITIONER_SET_GRAVITY || opcode == XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT) ARG_UINT(0);
    } else if (!strcmp(iface, "xdg_popup")) {
        if (opcode == XDG_POPUP_GRAB) { ARG_PTR(0); ARG_UINT(1); }
        else if (opcode == XDG_POPUP_REPOSITION) { ARG_PTR(0); ARG_UINT(1); }
    } else if (!strcmp(iface, "zxdg_decoration_manager_v1")) {
        if (opcode == ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) { SKIP_NEW_ID(); ARG_PTR(0); }
    } else if (!strcmp(iface, "zxdg_toplevel_decoration_v1")) {
        if (opcode == ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE) ARG_UINT(0);
    }
#undef ARG_PTR
#undef ARG_INT
#undef ARG_UINT
#undef ARG_TEXT
#undef SKIP_NEW_ID
}

/** @brief Record a configure serial sent to an xdg_surface; the oldest is dropped beyond FAKE_SERIALS. */
static void fake_add_serial(unsigned index, uint32_t serial) {
    fake_proxy_t *proxy = &fake_proxies[index];
    if (proxy->serial_count == FAKE_SERIALS) {
        memmove(proxy->serials, proxy->serials + 1, sizeof proxy->serials[0] * (FAKE_SERIALS - 1));
        proxy->serial_count--;
    }
    proxy->serials[proxy->serial_count++] = serial;
}

/** @brief Consume an outstanding configure serial. Returns 0 when the serial was never sent or already acknowledged. */
static int fake_take_serial(unsigned index, uint32_t serial) {
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
static void fake_assign_role(unsigned surface, uint8_t role) {
    if (surface == FAKE_NONE) return;
    if (fake_proxies[surface].role != FAKE_ROLE_NONE) ++fake_protocol_errors;
    fake_proxies[surface].role = role;
}

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)version;
    unsigned index = fake_index(proxy);
    int destroying = (flags & WL_MARSHAL_FLAG_DESTROY) != 0;
    int dead = index == FAKE_NONE || !fake_proxies[index].live;
    const char *iface = index == FAKE_NONE ? NULL : fake_proxies[index].iface;
    /* A destroy on a destroyed object is counted as a double destroy below, not as a request. */
    if (dead && !destroying) { ++fake_protocol_errors; ++fake_dead_requests; }
    static fake_request_t scratch;
    fake_request_t *request = fake_request_count < FAKE_REQUESTS ? &fake_requests[fake_request_count++] : &scratch;
    memset(request, 0, sizeof *request);
    request->proxy = index;
    request->iface = iface;
    request->opcode = opcode;
    request->destroy = (uint8_t)destroying;
    va_list ap;
    va_start(ap, flags);
    if (iface) decode_args(request, iface, opcode, &ap);
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
    unsigned created = fake_new(interface->name);
    /* Record what the new object was built on, from the arguments decoded above. */
    if (iface && !strcmp(iface, "xdg_wm_base") && opcode == XDG_WM_BASE_GET_XDG_SURFACE) {
        fake_proxies[created].aux = (unsigned)request->a0;
    }
    if (iface && !strcmp(iface, "xdg_surface") && opcode == XDG_SURFACE_GET_TOPLEVEL) {
        fake_proxies[created].aux = index;
        fake_assign_role(index, FAKE_ROLE_TOPLEVEL);
    }
    if (iface && !strcmp(iface, "xdg_surface") && opcode == XDG_SURFACE_GET_POPUP) {
        fake_proxies[created].aux = index;
        fake_proxies[created].aux2 = (unsigned)request->a0;
        fake_assign_role(index, FAKE_ROLE_POPUP);
    }
    if (iface && !strcmp(iface, "zxdg_decoration_manager_v1")) {
        fake_proxies[created].aux = (unsigned)request->a0;
    }
    return (struct wl_proxy *)fake_object(created);
}

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    (void)proxy;
    return fake_version;
}

int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = fake_index(proxy);
    if (index == FAKE_NONE || !fake_proxies[index].live) {
        ++fake_protocol_errors;
        ++fake_dead_requests;
        return -1;
    }
    if (fake_fail_listener && !strcmp(fake_proxies[index].iface, fake_fail_listener)) return -1;
    /* libwayland refuses a second listener on the same proxy. */
    if (fake_proxies[index].listener != NULL) return -1;
    fake_proxies[index].listener = (const void *)implementation;
    fake_proxies[index].data = data;
    return 0;
}

void __wrap_wl_proxy_destroy(struct wl_proxy *proxy) {
    fake_destroy_index(fake_index(proxy));
}

/* ---- compositor events ------------------------------------------------------------------------- */

static void deliver_surface_configure(unsigned index, uint32_t serial) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    fake_add_serial(index, serial);
    const struct xdg_surface_listener *listener = fake_proxies[index].listener;
    listener->configure(fake_proxies[index].data, (struct xdg_surface *)fake_object(index), serial);
}

static void deliver_toplevel_configure(unsigned index, int32_t width, int32_t height, const uint32_t *states, size_t count) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    const struct xdg_toplevel_listener *listener = fake_proxies[index].listener;
    struct wl_array array;
    array.size = count * sizeof *states;
    array.alloc = array.size;
    array.data = (void *)states;
    listener->configure(fake_proxies[index].data, (struct xdg_toplevel *)fake_object(index), width, height, &array);
}

static void deliver_toplevel_close(unsigned index) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    const struct xdg_toplevel_listener *listener = fake_proxies[index].listener;
    listener->close(fake_proxies[index].data, (struct xdg_toplevel *)fake_object(index));
}

/** @brief Index of the nth proxy created with interface `iface` (0-based), or FAKE_NONE. */
static unsigned fake_nth(const char *iface, unsigned nth) {
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, iface)) {
            if (nth == 0) return index;
            --nth;
        }
    }
    return FAKE_NONE;
}

static void deliver_decoration_configure(unsigned index, uint32_t mode) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    const struct zxdg_toplevel_decoration_v1_listener *listener = fake_proxies[index].listener;
    listener->configure(fake_proxies[index].data, (struct zxdg_toplevel_decoration_v1 *)fake_object(index), mode);
}

static void deliver_popup_configure(unsigned index, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    const struct xdg_popup_listener *listener = fake_proxies[index].listener;
    listener->configure(fake_proxies[index].data, (struct xdg_popup *)fake_object(index), x, y, width, height);
}

static void deliver_popup_repositioned(unsigned index, uint32_t token) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    const struct xdg_popup_listener *listener = fake_proxies[index].listener;
    listener->repositioned(fake_proxies[index].data, (struct xdg_popup *)fake_object(index), token);
}

static void deliver_popup_done(unsigned index) {
    if (fake_proxies[index].listener == NULL || !fake_proxies[index].live) return;
    fake_grab_pop(index);
    const struct xdg_popup_listener *listener = fake_proxies[index].listener;
    listener->popup_done(fake_proxies[index].data, (struct xdg_popup *)fake_object(index));
}

/* ---- request queries --------------------------------------------------------------------------- */

/** @brief Index of the first recorded request of `iface`/`opcode` at or after `from`, or -1. */
static int fake_find(const char *iface, uint32_t opcode, unsigned from) {
    for (unsigned index = from; index < fake_request_count; ++index) {
        if (fake_requests[index].iface && !strcmp(fake_requests[index].iface, iface) && fake_requests[index].opcode == opcode) return (int)index;
    }
    return -1;
}

static unsigned fake_count_requests(const char *iface, uint32_t opcode) {
    unsigned count = 0;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        if (fake_requests[index].iface && !strcmp(fake_requests[index].iface, iface) && fake_requests[index].opcode == opcode) ++count;
    }
    return count;
}

/** @brief Proxy index of the object a request was sent on, or FAKE_NONE. */
static unsigned fake_proxy_of_first(const char *iface, uint32_t opcode, unsigned from) {
    int index = fake_find(iface, opcode, from);
    return index < 0 ? FAKE_NONE : fake_requests[index].proxy;
}

#endif
