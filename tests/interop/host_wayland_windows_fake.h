/**
 * @file host_wayland_windows_fake.h
 * @brief Recording Wayland double for host_wayland_windows tests.
 *
 * Included by a test translation unit after the module source. Fake proxies live in a fixed array, so
 * their addresses map back to indexes. Every request is recorded with its decoded arguments. Creation and
 * destruction are counted, so a test can assert that nothing leaked. Listeners are stored and invoked by
 * the test, which lets it deliver compositor events, including late ones for destroyed objects. The
 * --wrap link flags in the lane rule route the generated wrappers here, as in tests/av/wayland_state.c.
 */
#ifndef HostWaylandWindowsFakeH
#define HostWaylandWindowsFakeH

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAKE_PROXIES 512u
#define FAKE_REQUESTS 4096u
#define FAKE_NONE 0xFFFFFFFFu

typedef struct fake_proxy_t {
    const char *iface; /* NULL for an unused slot */
    uint8_t live;
    const void *listener;
    void *data;
} fake_proxy_t;

typedef struct fake_request_t {
    unsigned proxy; /* FAKE_NONE for a proxy the fake does not own */
    const char *iface;
    uint32_t opcode;
    uint8_t destroy;
    long long a0, a1, a2, a3;
    char text[1100];
} fake_request_t;

static uintptr_t fake_storage[FAKE_PROXIES];
static fake_proxy_t fake_proxies[FAKE_PROXIES];
static unsigned fake_proxy_count;
static unsigned fake_creates, fake_destroys, fake_double_destroys;
static fake_request_t fake_requests[FAKE_REQUESTS];
static unsigned fake_request_count;
static const char *fake_fail_create; /* interface whose creation returns NULL */
static unsigned fake_fail_create_skip; /* creations of that interface allowed before failing */
static const char *fake_fail_listener; /* interface whose listener registration fails */
static uint32_t fake_version = 5;

static unsigned fake_index(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)fake_storage;
    if (address < base || address >= base + sizeof fake_storage || (address - base) % sizeof fake_storage[0]) return FAKE_NONE;
    unsigned index = (unsigned)((address - base) / sizeof fake_storage[0]);
    return index < fake_proxy_count ? index : FAKE_NONE;
}

static void *fake_object(unsigned index) {
    return (void *)&fake_storage[index];
}

static unsigned fake_new(const char *iface) {
    if (fake_proxy_count >= FAKE_PROXIES) __builtin_trap();
    unsigned index = fake_proxy_count++;
    fake_proxies[index].iface = iface;
    fake_proxies[index].live = 1;
    fake_proxies[index].listener = NULL;
    fake_proxies[index].data = NULL;
    ++fake_creates;
    return index;
}

static void fake_destroy_index(unsigned index) {
    if (index == FAKE_NONE) return;
    if (!fake_proxies[index].live) { ++fake_double_destroys; return; }
    fake_proxies[index].live = 0;
    ++fake_destroys;
}

static void fake_begin(void) {
    memset(fake_storage, 0, sizeof fake_storage);
    memset(fake_proxies, 0, sizeof fake_proxies);
    memset(fake_requests, 0, sizeof fake_requests);
    fake_proxy_count = 0;
    fake_creates = fake_destroys = fake_double_destroys = 0;
    fake_request_count = 0;
    fake_fail_create = NULL;
    fake_fail_create_skip = 0;
    fake_fail_listener = NULL;
    fake_version = 5;
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

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)version;
    unsigned index = fake_index(proxy);
    const char *iface = index == FAKE_NONE ? NULL : fake_proxies[index].iface;
    if (fake_request_count < FAKE_REQUESTS) {
        fake_request_t *request = &fake_requests[fake_request_count++];
        request->proxy = index;
        request->iface = iface;
        request->opcode = opcode;
        request->destroy = (flags & WL_MARSHAL_FLAG_DESTROY) != 0;
        va_list ap;
        va_start(ap, flags);
        if (iface) decode_args(request, iface, opcode, &ap);
        va_end(ap);
    }
    if (flags & WL_MARSHAL_FLAG_DESTROY) fake_destroy_index(index);
    if (interface == NULL) return NULL;
    if (fake_fail_create && !strcmp(interface->name, fake_fail_create)) {
        if (fake_fail_create_skip == 0) return NULL;
        --fake_fail_create_skip;
    }
    return (struct wl_proxy *)fake_object(fake_new(interface->name));
}

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    (void)proxy;
    return fake_version;
}

int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = fake_index(proxy);
    if (index == FAKE_NONE) return -1;
    if (fake_fail_listener && !strcmp(fake_proxies[index].iface, fake_fail_listener)) return -1;
    fake_proxies[index].listener = (const void *)implementation;
    fake_proxies[index].data = data;
    return 0;
}

void __wrap_wl_proxy_destroy(struct wl_proxy *proxy) {
    fake_destroy_index(fake_index(proxy));
}

/* ---- compositor events ------------------------------------------------------------------------- */

static void deliver_surface_configure(unsigned index, uint32_t serial) {
    const struct xdg_surface_listener *listener = fake_proxies[index].listener;
    listener->configure(fake_proxies[index].data, (struct xdg_surface *)fake_object(index), serial);
}

static void deliver_toplevel_configure(unsigned index, int32_t width, int32_t height, const uint32_t *states, size_t count) {
    const struct xdg_toplevel_listener *listener = fake_proxies[index].listener;
    struct wl_array array;
    array.size = count * sizeof *states;
    array.alloc = array.size;
    array.data = (void *)states;
    listener->configure(fake_proxies[index].data, (struct xdg_toplevel *)fake_object(index), width, height, &array);
}

static void deliver_toplevel_close(unsigned index) {
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
    const struct zxdg_toplevel_decoration_v1_listener *listener = fake_proxies[index].listener;
    listener->configure(fake_proxies[index].data, (struct zxdg_toplevel_decoration_v1 *)fake_object(index), mode);
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
