/**
 * @file host_wayland_core_fake.h
 * @brief Recording Wayland double for host_wayland_core tests.
 *
 * Included by host_wayland_core_test.c after the module source. The double replaces the libwayland entry points the
 * core calls, through `-Wl,--wrap`: the proxy marshal, listener, destroy and version functions, and the display
 * functions (connect, disconnect, fd, error, dispatch, prepare/read, flush, roundtrip). Inline generated wrappers
 * (wl_registry_bind, xdg_wm_base_pong, wl_seat_release and so on) reach the double through wl_proxy_marshal_flags.
 *
 * Model:
 *  - Every object is a fake proxy with a fixed address in fake_storage. Addresses are never reused, so a stale
 *    pointer always names its own (dead) record.
 *  - Requests on a dead or unknown proxy count as protocol errors; destroying a dead proxy counts as a double
 *    destroy. The test entry point requires both counters to stay zero.
 *  - Compositor events are queued with fake_queue_* and delivered, in order, by wl_display_dispatch_pending and
 *    wl_display_roundtrip, as libwayland delivers them. Delivery calls the registered listener directly.
 *  - Every destroy of a live proxy appends its interface name to fake_log; a teardown sequence is therefore a log.
 *  - The event queue is the default queue: wl_display_prepare_read fails with EAGAIN while it is nonempty, as libwayland
 *    does. fake_refill re-queues one no-op event during the next dispatch_pending, which exercises the retry path.
 */
#ifndef HostWaylandCoreFakeH
#define HostWaylandCoreFakeH

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FakeProxies 96u
#define FakeRequests 512u
#define FakeEvents 128u
#define FakeLog 128u
#define FakeNone 0xFFFFFFFFu
/** @brief Descriptor reported by wl_display_get_fd. */
#define FakeFd 7

/** @brief Outcome a libwayland call is told to produce. */
enum fake_mode_t {
    FakeOk = 0,
    FakeEagain, /**< Returns -1, errno EAGAIN, no display error. */
    FakeEintr, /**< Returns -1, errno EINTR, no display error. */
    FakeEio, /**< Returns -1, errno EIO, no display error. */
    FakeFault, /**< Returns -1, errno EIO, and wl_display_get_error becomes nonzero. */
    FakeFaultEagain /**< Returns -1, errno EAGAIN, and wl_display_get_error becomes nonzero. */
};

/** @brief Kinds of queued compositor events. */
enum fake_event_kind_t {
    FakeEventAnnounce, /**< wl_registry.global with name, iface, version. */
    FakeEventRemove, /**< wl_registry.global_remove with name. */
    FakeEventPing, /**< xdg_wm_base.ping with serial in `name`. */
    FakeEventNoop /**< Queued without a listener call; makes the queue nonempty. */
};

typedef struct fake_proxy_t {
    const char *iface; /* NULL never; every issued proxy has an interface */
    uint8_t live;
    uint32_t version; /* version the proxy was created or bound with */
    const void *listener; /* listener struct address, or NULL */
    void *data;
} fake_proxy_t;

typedef struct fake_request_t {
    unsigned proxy;
    const char *iface;
    uint32_t opcode;
    uint8_t destroy;
    long long a0;
    long long a1;
    char text[64]; /* registry bind: the interface name */
} fake_request_t;

typedef struct fake_event_t {
    enum fake_event_kind_t kind;
    uint32_t name;
    uint32_t version;
    const char *iface;
} fake_event_t;

static uintptr_t fake_storage[FakeProxies];
static fake_proxy_t fake_proxies[FakeProxies];
static unsigned fake_proxy_count;
static fake_request_t fake_requests[FakeRequests];
static unsigned fake_request_count;
static const char *fake_log[FakeLog];
static unsigned fake_log_count;
static fake_event_t fake_events[FakeEvents];
static unsigned fake_event_head;
static unsigned fake_event_tail;
static unsigned fake_refill; /* one no-op event is queued by each of the next N dispatch_pending calls */
static unsigned fake_prepare_force; /* prepare_read fails with EAGAIN this many more times, whatever the queue holds */
static int fake_prepare_errno; /* when nonzero, every prepare_read fails with this errno */
static enum fake_mode_t fake_read_mode;
static enum fake_mode_t fake_flush_mode;
static enum fake_mode_t fake_roundtrip_mode;
static int fake_display_error; /* what wl_display_get_error returns */
static unsigned fake_dispatch_calls;
static unsigned fake_dispatch_fail_at; /* the dispatch_pending call number that fails with EPROTO; 0 for none */
static unsigned fake_prepare_reads;
static unsigned fake_prepare_failures;
static unsigned fake_read_calls;
static unsigned fake_flushes;
static unsigned fake_roundtrips;
static unsigned fake_connects;
static unsigned fake_disconnects;
static const char *fake_connect_name;
static int fake_connect_fail;
static const char *fake_fail_create; /* interface whose proxy creation returns NULL */
static const char *fake_fail_listener; /* interface whose listener registration fails */
static unsigned fake_protocol_errors; /* requests on dead or unknown proxies, and other misuse; never reset */
static unsigned fake_double_destroys; /* destroys of dead proxies; never reset */

/** @brief Proxy index of the object `pointer` names, or FakeNone for a foreign or stale-address pointer. */
static unsigned fake_index(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)fake_storage;
    if (address < base || address >= base + sizeof fake_storage) return FakeNone;
    if ((address - base) % sizeof fake_storage[0]) return FakeNone;
    unsigned index = (unsigned)((address - base) / sizeof fake_storage[0]);
    return index < fake_proxy_count ? index : FakeNone;
}

/** @brief Fixed address of a proxy. */
static void *fake_object(unsigned index) {
    return (void *)&fake_storage[index];
}

/** @brief Create a live proxy. Addresses are never reused. */
static unsigned fake_new(const char *iface, uint32_t version) {
    if (fake_proxy_count >= FakeProxies) __builtin_trap();
    unsigned index = fake_proxy_count++;
    memset(&fake_proxies[index], 0, sizeof fake_proxies[index]);
    fake_proxies[index].iface = iface;
    fake_proxies[index].live = 1;
    fake_proxies[index].version = version;
    return index;
}

static void fake_log_push(const char *entry) {
    if (fake_log_count >= FakeLog) __builtin_trap();
    fake_log[fake_log_count++] = entry;
}

/** @brief Destroy a proxy. Logs its interface, marks it dead. Destroying a dead proxy is a double destroy. */
static void fake_destroy_index(unsigned index) {
    if (index == FakeNone) {
        ++fake_protocol_errors;
        return;
    }
    if (!fake_proxies[index].live) {
        ++fake_double_destroys;
        return;
    }
    fake_log_push(fake_proxies[index].iface);
    fake_proxies[index].live = 0;
}

/** @brief First live proxy with `iface`, or FakeNone. */
static unsigned fake_live_nth(const char *iface) {
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].live && !strcmp(fake_proxies[index].iface, iface)) return index;
    }
    return FakeNone;
}

/** @brief Version a proxy was created or bound with. Zero for a pointer the double never issued. */
static uint32_t fake_version_of(const void *object) {
    unsigned index = fake_index(object);
    return index == FakeNone ? 0 : fake_proxies[index].version;
}

static void fake_queue(enum fake_event_kind_t kind, uint32_t name, uint32_t version, const char *iface) {
    if (fake_event_tail >= FakeEvents) __builtin_trap();
    fake_event_t *event = &fake_events[fake_event_tail++];
    event->kind = kind;
    event->name = name;
    event->version = version;
    event->iface = iface;
}

/** @brief Queue a wl_registry.global for delivery at the next dispatch_pending or roundtrip. */
static void queue_announce(uint32_t name, const char *iface, uint32_t version) {
    fake_queue(FakeEventAnnounce, name, version, iface);
}

/** @brief Queue a wl_registry.global_remove for delivery at the next dispatch_pending or roundtrip. */
static void queue_remove(uint32_t name) {
    fake_queue(FakeEventRemove, name, 0, NULL);
}

/** @brief Queue an xdg_wm_base.ping with `serial` for delivery at the next dispatch_pending or roundtrip. */
static void queue_ping(uint32_t serial) {
    fake_queue(FakeEventPing, serial, 0, NULL);
}

static void fake_deliver(const fake_event_t *event) {
    unsigned index;
    switch (event->kind) {
    case FakeEventAnnounce:
        index = fake_live_nth("wl_registry");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct wl_registry_listener *)fake_proxies[index].listener)->global(fake_proxies[index].data, (struct wl_registry *)fake_object(index), event->name, event->iface, event->version);
        return;
    case FakeEventRemove:
        index = fake_live_nth("wl_registry");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct wl_registry_listener *)fake_proxies[index].listener)->global_remove(fake_proxies[index].data, (struct wl_registry *)fake_object(index), event->name);
        return;
    case FakeEventPing:
        index = fake_live_nth("xdg_wm_base");
        if (index == FakeNone || !fake_proxies[index].listener) return;
        ((const struct xdg_wm_base_listener *)fake_proxies[index].listener)->ping(fake_proxies[index].data, (struct xdg_wm_base *)fake_object(index), event->name);
        return;
    case FakeEventNoop:
        return;
    }
}

/** @brief Deliver every queued event in order. Returns how many were delivered. */
static unsigned fake_deliver_queued(void) {
    unsigned delivered = 0;
    while (fake_event_head < fake_event_tail) {
        fake_event_t event = fake_events[fake_event_head++];
        fake_deliver(&event);
        ++delivered;
    }
    return delivered;
}

/** @brief Apply a mode to a call that returns int: 0 for FakeOk, otherwise -1 with the errno and error the mode names. */
static int fake_fail_with(enum fake_mode_t mode) {
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

static struct wl_display *fake_make_display(void) {
    return (struct wl_display *)fake_object(fake_new("wl_display", 1));
}

/** @brief Reset all recording state for a fresh test. Protocol-error counters are not reset. */
static void fake_begin(void) {
    fake_proxy_count = 0;
    memset(fake_proxies, 0, sizeof fake_proxies);
    fake_request_count = 0;
    fake_log_count = 0;
    fake_event_head = 0;
    fake_event_tail = 0;
    fake_refill = 0;
    fake_prepare_force = 0;
    fake_prepare_errno = 0;
    fake_read_mode = FakeOk;
    fake_flush_mode = FakeOk;
    fake_roundtrip_mode = FakeOk;
    fake_display_error = 0;
    fake_dispatch_calls = 0;
    fake_dispatch_fail_at = 0;
    fake_prepare_reads = 0;
    fake_prepare_failures = 0;
    fake_read_calls = 0;
    fake_flushes = 0;
    fake_roundtrips = 0;
    fake_connects = 0;
    fake_disconnects = 0;
    fake_connect_name = NULL;
    fake_connect_fail = 0;
    fake_fail_create = NULL;
    fake_fail_listener = NULL;
}

/** @brief Number of recorded requests with `iface` and `opcode`. */
static unsigned fake_count_requests(const char *iface, uint32_t opcode) {
    unsigned count = 0;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        const fake_request_t *request = &fake_requests[index];
        if (request->iface && !strcmp(request->iface, iface) && request->opcode == opcode) ++count;
    }
    return count;
}

/** @brief Number of wl_registry.bind requests that named `interface`. */
static unsigned fake_count_binds(const char *interface) {
    unsigned count = 0;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        const fake_request_t *request = &fake_requests[index];
        if (!strcmp(request->iface, "wl_registry") && request->opcode == WL_REGISTRY_BIND && !strcmp(request->text, interface)) ++count;
    }
    return count;
}

static void fake_copy_text(char *destination, size_t capacity, const char *source) {
    size_t length = 0;
    while (source && length + 1 < capacity && source[length]) {
        destination[length] = source[length];
        ++length;
    }
    destination[length] = '\0';
}

/* ---- libwayland proxy entry points --------------------------------------------------------------- */

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    unsigned index = fake_index(proxy);
    int destroying = (flags & WL_MARSHAL_FLAG_DESTROY) != 0;
    if (index == FakeNone || !fake_proxies[index].live) {
        if (destroying) ++fake_double_destroys;
        else ++fake_protocol_errors;
        return NULL;
    }
    const char *iface = fake_proxies[index].iface;
    if (fake_request_count >= FakeRequests) __builtin_trap();
    fake_request_t *request = &fake_requests[fake_request_count++];
    memset(request, 0, sizeof *request);
    request->proxy = index;
    request->iface = iface;
    request->opcode = opcode;
    request->destroy = (uint8_t)destroying;
    va_list ap;
    va_start(ap, flags);
    if (!strcmp(iface, "wl_registry") && opcode == WL_REGISTRY_BIND) {
        request->a0 = (long long)va_arg(ap, uint32_t);
        fake_copy_text(request->text, sizeof request->text, va_arg(ap, const char *));
        request->a1 = (long long)va_arg(ap, uint32_t);
    } else if (!strcmp(iface, "xdg_wm_base") && opcode == XDG_WM_BASE_PONG) {
        request->a0 = (long long)va_arg(ap, uint32_t);
    }
    va_end(ap);
    if (destroying) fake_destroy_index(index);
    if (!interface) return NULL;
    if (fake_fail_create && !strcmp(interface->name, fake_fail_create)) return NULL;
    return (struct wl_proxy *)fake_object(fake_new(interface->name, version));
}

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    unsigned index = fake_index(proxy);
    if (index == FakeNone) {
        ++fake_protocol_errors;
        return 0;
    }
    return fake_proxies[index].version;
}

int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = fake_index(proxy);
    if (index == FakeNone || !fake_proxies[index].live) {
        ++fake_protocol_errors;
        return -1;
    }
    if (fake_fail_listener && !strcmp(fake_proxies[index].iface, fake_fail_listener)) return -1;
    /* libwayland refuses a second listener on one proxy. */
    if (fake_proxies[index].listener) return -1;
    fake_proxies[index].listener = implementation;
    fake_proxies[index].data = data;
    return 0;
}

void __wrap_wl_proxy_destroy(struct wl_proxy *proxy) {
    fake_destroy_index(fake_index(proxy));
}

/* ---- libwayland display entry points ------------------------------------------------------------- */

struct wl_display *__wrap_wl_display_connect(const char *name) {
    ++fake_connects;
    fake_connect_name = name;
    if (fake_connect_fail) return NULL;
    return fake_make_display();
}

void __wrap_wl_display_disconnect(struct wl_display *display) {
    unsigned index = fake_index(display);
    ++fake_disconnects;
    fake_log_push("wl_display_disconnect");
    if (index == FakeNone || !fake_proxies[index].live) {
        ++fake_double_destroys;
        return;
    }
    fake_proxies[index].live = 0;
}

int __wrap_wl_display_get_fd(struct wl_display *display) {
    (void)display;
    return FakeFd;
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
    unsigned delivered = fake_deliver_queued();
    if (fake_refill) {
        --fake_refill;
        fake_queue(FakeEventNoop, 0, 0, NULL);
    }
    return (int)delivered;
}

int __wrap_wl_display_prepare_read(struct wl_display *display) {
    (void)display;
    ++fake_prepare_reads;
    if (fake_prepare_errno) {
        errno = fake_prepare_errno;
        ++fake_prepare_failures;
        return -1;
    }
    if (fake_prepare_force) {
        --fake_prepare_force;
        errno = EAGAIN;
        ++fake_prepare_failures;
        return -1;
    }
    if (fake_event_tail > fake_event_head) {
        errno = EAGAIN;
        ++fake_prepare_failures;
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

#endif
