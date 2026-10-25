/** @file wayland_state.c @brief Real Wayland callbacks for V1/V2 lifecycle and V3 input lease integration.
 * Includes the implementation to exercise private compositor callbacks without
 * fabricating a display connection; native platform tests cover real protocol I/O.
 */
#include <wayland-client.h>
#include <stdio.h>
#include "xdg_shell_client.h"
#include "xdg_decoration_client.h"
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <string.h>
/* Native protocol double: production callbacks and message dispatch are real;
 * every Wayland request is counted before it could reach a compositor. */
static unsigned native_effects, proxy_count;
static uintptr_t proxy_storage[4096];
static const struct wl_callback_listener *callback_listeners[4096];
static void *callback_contexts[4096];
static uint8_t callback_proxies[4096], destroyed_proxies[4096];
static uint64_t now_ms = 1000;
static int clock_failure, sync_failure, listener_failure, flush_error;
static unsigned flushes, syncs, proxy_destroys, pending_dispatches, disconnects;
static void (*during_drain)(void);
/* Request recorder: interface of each fake proxy plus the decoded arguments of
 * the requests the decoration/title tests assert on. */
static const char *proxy_names[4096];
static unsigned title_requests, decoration_modes, destroy_count;
static char last_title[256];
static uint32_t last_decoration_mode;
static const char *destroy_log[16];
static const struct zxdg_toplevel_decoration_v1_listener *decoration_listeners[4096];
static const struct xdg_surface_listener *surface_listeners[4096];
static void *decoration_contexts[4096], *surface_contexts[4096];
static int decoration_listener_failure, decoration_creation_failure;
static unsigned surface_acks, decoration_destroy_ack;
static uint32_t last_surface_serial;
static unsigned proxy_index(struct wl_proxy *proxy) {
    uintptr_t address = (uintptr_t)proxy;
    uintptr_t base = (uintptr_t)proxy_storage;
    if (address < base || address >= base + sizeof(proxy_storage) ||
        (address - base) % sizeof(proxy_storage[0])) __builtin_trap();
    return (unsigned)((address - base) / sizeof(proxy_storage[0]));
}
/* Interface name of a fake proxy, NULL for test-local sentinel pointers. */
static const char *proxy_name(struct wl_proxy *proxy) {
    uintptr_t address = (uintptr_t)proxy, base = (uintptr_t)proxy_storage;
    if (address < base || address >= base + sizeof(proxy_storage) ||
        (address - base) % sizeof(proxy_storage[0])) return NULL;
    return proxy_names[(address - base) / sizeof(proxy_storage[0])];
}
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)version;
    ++native_effects;
    const char *target = proxy_name(proxy);
    if (target && (flags & WL_MARSHAL_FLAG_DESTROY) && destroy_count < 16)
        destroy_log[destroy_count++] = target;
    if (target && !strcmp(target, "zxdg_toplevel_decoration_v1") &&
        (flags & WL_MARSHAL_FLAG_DESTROY)) decoration_destroy_ack = surface_acks;
    if (target && !strcmp(target, "xdg_surface") && opcode == XDG_SURFACE_ACK_CONFIGURE) {
        va_list arguments;
        va_start(arguments, flags);
        last_surface_serial = va_arg(arguments, uint32_t);
        va_end(arguments);
        ++surface_acks;
    }
    if (target && !strcmp(target, "xdg_toplevel") && opcode == XDG_TOPLEVEL_SET_TITLE) {
        va_list arguments;
        va_start(arguments, flags);
        const char *title = va_arg(arguments, const char *);
        va_end(arguments);
        snprintf(last_title, sizeof(last_title), "%s", title);
        ++title_requests;
    }
    if (target && !strcmp(target, "zxdg_toplevel_decoration_v1") &&
        opcode == ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE) {
        va_list arguments;
        va_start(arguments, flags);
        last_decoration_mode = va_arg(arguments, uint32_t);
        va_end(arguments);
        ++decoration_modes;
    }
    if (!interface) return NULL;
    if (!strcmp(interface->name, "zxdg_toplevel_decoration_v1") &&
        decoration_creation_failure) return NULL;
    int callback = !strcmp(interface->name, "wl_callback");
    if (callback) {
        ++syncs;
        if (sync_failure) return NULL;
    }
    if (proxy_count >= 4096) __builtin_trap();
    callback_proxies[proxy_count] = (uint8_t)callback;
    proxy_names[proxy_count] = interface->name;
    return (struct wl_proxy *)&proxy_storage[proxy_count++];
}
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) { (void)proxy; return 1; }
int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = proxy_index(proxy);
    const char *name = proxy_names[index];
    if (name && !strcmp(name, "zxdg_toplevel_decoration_v1")) {
        if (decoration_listener_failure) return -1;
        decoration_listeners[index] = (const struct zxdg_toplevel_decoration_v1_listener *)implementation;
        decoration_contexts[index] = data;
    }
    if (name && !strcmp(name, "xdg_surface")) {
        surface_listeners[index] = (const struct xdg_surface_listener *)implementation;
        surface_contexts[index] = data;
    }
    if (callback_proxies[index]) {
        if (listener_failure) return -1;
        callback_listeners[index] = (const struct wl_callback_listener *)implementation;
        callback_contexts[index] = data;
    }
    return 0;
}
void __wrap_wl_proxy_destroy(struct wl_proxy *proxy) {
    ++proxy_destroys;
    if (proxy) destroyed_proxies[proxy_index(proxy)] = 1;
}
int __wrap_wl_display_flush(struct wl_display *display) {
    (void)display; ++flushes;
    if (flush_error) { errno = flush_error; return -1; }
    return 0;
}
int __wrap_clock_gettime(clockid_t clock, struct timespec *now) {
    (void)clock;
    if (clock_failure) return -1;
    now->tv_sec = (time_t)(now_ms / 1000);
    now->tv_nsec = (long)(now_ms % 1000) * 1000000;
    return 0;
}
int __wrap_wl_display_dispatch_pending(struct wl_display *display) {
    (void)display; ++pending_dispatches;
    if (during_drain) during_drain();
    return 0;
}
void __wrap_wl_display_disconnect(struct wl_display *display) { (void)display; ++disconnects; }
#include "av_wayland.c"
#include <assert.h>
#include <fcntl.h>
static av_message_t delivered, delivered_log[1024];
static unsigned deliveries;
static int fail_delivery;
static unsigned fail_delivery_at;
static void done(void *context) { (void)context; }
static int receive_request(const av_message_t *message, void *context) {
    assert(context == &deliveries);
    delivered = *message;
    assert(deliveries < 1024);
    delivered_log[deliveries++] = *message;
    return fail_delivery || (fail_delivery_at && deliveries == fail_delivery_at) ? -1 : 0;
}
static void lifecycle_callbacks(void);
static void live_repeat(void);
static void decoration_callbacks(void);
static void lease_callbacks(void);
static void lease_failures(void);
static void lease_lifecycle(void);
static void input_callbacks(void) {
    deliveries = 0; fail_delivery = 0;
    av_wayland_t client = {.request = receive_request, .request_context = &deliveries};
    video_window_t *first = &client.windows[0], *second = &client.windows[1];
    *first = (video_window_t){.client = &client, .geometry = {.window_id = 1},
        .incarnation = 10, .surface = (struct wl_surface *)(uintptr_t)1,
        .input_width = 320, .input_height = 200};
    *second = (video_window_t){.client = &client, .geometry = {.window_id = 2},
        .incarnation = 20, .surface = (struct wl_surface *)(uintptr_t)2,
        .input_width = 640, .input_height = 480};
    uint32_t held = 30;
    struct wl_array keys = {.size = sizeof(held), .data = &held};
    pointer_enter(&client, NULL, 0, first->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(deliveries == 0); /* no accepted physical-mode keymap yet */
    int descriptors[2]; assert(pipe(descriptors) == 0);
    keyboard_keymap(&client, NULL, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, descriptors[0], 10);
    assert(fcntl(descriptors[0], F_GETFD) == -1 && errno == EBADF); close(descriptors[1]);
    first->incarnation = 0;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(deliveries == 0 && !client.keyboard_window); /* explicit legacy view-only */
    first->incarnation = 10;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(deliveries == 2 && delivered.type == MsgInputPointer && delivered.sequence == 10);
    keyboard_key(&client, NULL, 0, 0, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    keyboard_key(&client, NULL, 0, 0, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
    assert(deliveries == 2); /* do not import a held key from the Linux application */
    keyboard_key(&client, NULL, 0, 0, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    assert(deliveries == 3 && delivered.type == MsgInputKey && delivered.width == 30);
    keyboard_key(&client, NULL, 0, 0, 128, 1);
    keyboard_key(&client, NULL, 0, 0, 84, 1);
    keyboard_key(&client, NULL, 0, 0, 30, 2);
    assert(deliveries == 3);
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == 4 && delivered.type == MsgInputButton && delivered.flags == 1);
    pointer_motion(&client, NULL, 0, -1, wl_fixed_from_int(20));
    pointer_button(&client, NULL, 0, 0, 0x111, 1);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_frame(&client, NULL);
    assert(deliveries == 4); /* bounds apply to new downs and wheel during implicit grabs */
    pointer_button(&client, NULL, 0, 0, 0x110, 0);
    assert(deliveries == 5 && delivered.flags == 0); /* releases always escape */
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(320), wl_fixed_from_int(20));
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == 5);
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(319), wl_fixed_from_int(199));
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_frame(&client, NULL);
    assert(deliveries == 7 && delivered.type == MsgInputWheel && delivered.y == -120);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(10));
    pointer_frame(&client, NULL);
    assert(deliveries == 8 && delivered.x == 120);
    /* A huge frame is clamped to 12000 units and sent as ten ordered 1200-unit messages. */
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, INT32_MAX);
    pointer_frame(&client, NULL);
    assert(deliveries == 18 && delivered.type == MsgInputWheel && delivered.y == -1200 && delivered.x == 0);
    pointer_button(&client, NULL, 0, 0, 0x115, 1); /* BTN_FORWARD is dropped */
    assert(deliveries == 18);
    deliveries = 8; /* Continue the original sequence numbering after the chunk check. */
    pointer_leave(&client, NULL, 0, first->surface);
    assert(deliveries == 9 && delivered.type == MsgInputRelease && delivered.flags == AvInputReleaseButtons);
    pointer_enter(&client, NULL, 0, second->surface, wl_fixed_from_int(15), wl_fixed_from_int(25));
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == 9);
    keys.size = 0;
    keyboard_enter(&client, NULL, 0, second->surface, &keys);
    assert(deliveries == 12 && client.keyboard_window == second && delivered.sequence == 20);
    keyboard_leave(&client, NULL, 0, first->surface);
    assert(deliveries == 12 && client.keyboard_window == second);
    keyboard_leave(&client, NULL, 0, second->surface);
    assert(deliveries == 13 && !client.keyboard_window && delivered.type == MsgInputFocus && !delivered.flags);
    keyboard_enter(&client, NULL, 0, second->surface, &keys);
    unsigned before = deliveries;
    second->surface = NULL; /* fake proxy never passed to libwayland */
    retire_window(second);
    assert(deliveries == before && !client.keyboard_window && !client.pointer_window);
    client.input_serial = UINT32_MAX;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(client.delivery_failed && deliveries == before);
    client.delivery_failed = 0; client.input_serial = 0; client.keyboard_window = NULL;
    keys.size = 1;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(client.delivery_failed && !client.keyboard_window);
    client.delivery_failed = 0; keys.size = 0; fail_delivery = 1;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(client.delivery_failed);
    fail_delivery = 0; before = deliveries;
    keyboard_leave(&client, NULL, 0, first->surface);
    assert(!client.keyboard_window && deliveries == before); /* full queue is fatal; no dropped-release success */
    client.delivery_failed = 0; client.shutting_down = 1;
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(!client.keyboard_window && deliveries == before); /* teardown dispatch cannot re-arm input */
}
/* Live host repeat. The fake clock is whole milliseconds; reducer deadlines are ns. */
static void live_ready(av_wayland_t *client, video_window_t **window) {
    *client = (av_wayland_t){.request = receive_request, .request_context = &deliveries};
    assert(waddle_keyboard_repeat_init(&client->repeat) == WaddleKeyboardRepeatOk);
    *window = &client->windows[0];
    **window = (video_window_t){.client = client, .geometry = {.window_id = 1}, .incarnation = 10,
        .surface = (struct wl_surface *)(uintptr_t)1, .input_width = 320, .input_height = 200};
    int descriptors[2]; assert(pipe(descriptors) == 0);
    keyboard_keymap(client, NULL, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, descriptors[0], 10);
    close(descriptors[1]);
}
static void live_enter(av_wayland_t *client, video_window_t *window) {
    struct wl_array none = {0};
    keyboard_enter(client, NULL, 0, window->surface, &none);
    assert(client->keyboard_window == window);
}
static void live_key(av_wayland_t *client, uint32_t key, int pressed) {
    keyboard_key(client, NULL, 0, 0, key, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}
static int live_next(av_wayland_t *client, uint64_t *deadline) {
    uint8_t present = 0;
    assert(waddle_keyboard_repeat_next_deadline(&client->repeat, &present, deadline) == WaddleKeyboardRepeatOk);
    return present;
}
static void live_repeat_timing(void) {
    av_wayland_t client; video_window_t *window;
    live_ready(&client, &window);
    now_ms = 1000;
    keyboard_repeat(&client, NULL, 25, 100);
    live_enter(&client, window);
    deliveries = 0;
    live_key(&client, 30, 1);
    assert(deliveries == 1 && delivered.type == MsgInputKey && delivered.flags == 1 && delivered.width == 30);
    assert(av_wayland_timeout(&client, 1000) == 100); /* delay 100 ms, rounded up to whole ms */
    now_ms = 1099;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 1);
    assert(av_wayland_timeout(&client, 1000) == 1);
    now_ms = 1100;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 2);
    assert(delivered.type == MsgInputKey && delivered.flags == 1 && delivered.width == 30);
    assert(av_wayland_timeout(&client, 1000) == 40); /* period ceil(1e9/25) ns = 40 ms */
    now_ms = 1139;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 2);
    now_ms = 1140;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 3 && delivered.width == 30);
    /* A key up stops the repeat exactly; nothing further is due. */
    live_key(&client, 30, 0);
    assert(deliveries == 4 && delivered.flags == 0);
    now_ms = 2000;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 4);
    /* Rounding: rate 3 has a 333333334 ns period; the poll waits 334 ms, never less. */
    keyboard_repeat(&client, NULL, 3, 100);
    now_ms = 2000;
    live_key(&client, 31, 1);
    assert(av_wayland_timeout(&client, 1000) == 100);
    now_ms = 2100;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 6 && delivered.width == 31);
    assert(av_wayland_timeout(&client, 1000) == 334);
    now_ms = 2433;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 6);
    now_ms = 2434;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 7);
    live_key(&client, 31, 0);
    puts("AV Wayland live repeat: exact delay, period, ceil rounding and key-up stop passed");
}
static void live_repeat_info(void) {
    av_wayland_t client; video_window_t *window;
    live_ready(&client, &window);
    now_ms = 3000;
    keyboard_repeat(&client, NULL, 25, 100);
    keyboard_repeat(&client, NULL, -1, 50);      /* negative rate rejected, timing kept */
    keyboard_repeat(&client, NULL, 25, -1);      /* negative delay rejected */
    keyboard_repeat(&client, NULL, 1001, 100);   /* rate above 1000 Hz rejected */
    keyboard_repeat(&client, NULL, 25, 60001);   /* delay above 60000 ms rejected */
    live_enter(&client, window);
    deliveries = 0;
    live_key(&client, 30, 1);
    uint64_t deadline = 0;
    assert(live_next(&client, &deadline) && deadline == 3100000000ull);
    keyboard_repeat(&client, NULL, 0, 100);      /* rate zero disables repeat */
    assert(!live_next(&client, &deadline));
    now_ms = 3200;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 1);
    keyboard_repeat(&client, NULL, 25, 100);
    live_key(&client, 30, 0);
    live_key(&client, 31, 1);
    assert(live_next(&client, &deadline) && deadline == 3200000000ull + 100000000ull);
    now_ms = 3300;
    assert(av_wayland_timeout(&client, 0) == 0);
    assert(live_next(&client, &deadline) && deadline == 3300000000ull + 40000000ull);
    live_key(&client, 31, 0);
    puts("AV Wayland repeat_info: negative and out-of-range values rejected, rate zero disables, timing kept passed");
}
static void live_repeat_cancel(void) {
    av_wayland_t client; video_window_t *window;
    uint64_t deadline = 0;
    /* Leave cancels the repeat: no later repeat even when the key was never released. */
    live_ready(&client, &window);
    now_ms = 4000;
    keyboard_repeat(&client, NULL, 25, 100);
    live_enter(&client, window);
    deliveries = 0;
    live_key(&client, 30, 1);
    assert(live_next(&client, &deadline));
    keyboard_leave(&client, NULL, 0, window->surface);
    assert(!live_next(&client, &deadline));
    unsigned after_leave = deliveries;
    now_ms = 4500;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == after_leave);
    /* Keymap format change clears focus and cancels the repeat. */
    live_ready(&client, &window);
    keyboard_repeat(&client, NULL, 25, 100);
    live_enter(&client, window);
    live_key(&client, 30, 1);
    keyboard_keymap(&client, NULL, 0, -1, 0); /* not XKB_V1: focus cleared */
    assert(!live_next(&client, &deadline) && !client.keyboard_window);
    /* Lease revocation disables routes, which cancels the repeat. */
    live_ready(&client, &window);
    client.identity.mode = AvIdentityLease;
    client.lease.phase = AvLeaseForwarding;
    client.keyboard_window = window;
    assert(waddle_keyboard_repeat_focus(&client.repeat, 1, 10) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_configure(&client.repeat, 1, 25, 100) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_key(&client.repeat, 30, 1, 1, 1000000000ull) == WaddleKeyboardRepeatOk);
    assert(live_next(&client, &deadline));
    disable_routes(&client);
    assert(!live_next(&client, &deadline) && !client.keyboard_window);
    /* A delivery failure on a repeat cancels the repeat and fails the input path. */
    live_ready(&client, &window);
    keyboard_repeat(&client, NULL, 25, 100);
    live_enter(&client, window);
    live_key(&client, 30, 1);
    now_ms = 4700;
    fail_delivery_at = deliveries + 1;
    assert(av_wayland_timeout(&client, 0) == -1 && client.delivery_failed);
    fail_delivery_at = 0;
    assert(!live_next(&client, &deadline));
    puts("AV Wayland repeat cancel: leave, focus clear and failed delivery stop repeats passed");
}
static void live_repeat_suppressed_and_rollback(void) {
    av_wayland_t client; video_window_t *window;
    uint64_t deadline = 0;
    /* A key held at focus enter is suppressed, so it never arms a repeat. */
    live_ready(&client, &window);
    now_ms = 5000;
    keyboard_repeat(&client, NULL, 25, 100);
    uint32_t held = 30;
    struct wl_array keys = {.size = sizeof(held), .data = &held};
    keyboard_enter(&client, NULL, 0, window->surface, &keys);
    deliveries = 0;
    live_key(&client, 30, 1);
    assert(deliveries == 0 && !live_next(&client, &deadline));
    now_ms = 5500;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 0);
    /* A clock that moves backward disarms the repeat: none is delivered later. */
    live_ready(&client, &window);
    now_ms = 6000;
    keyboard_repeat(&client, NULL, 25, 100);
    live_enter(&client, window);
    deliveries = 0;
    live_key(&client, 30, 1);
    now_ms = 5900;
    assert(av_wayland_timeout(&client, 1000) == 1000 && deliveries == 1); /* only the forwarded down */
    assert(!live_next(&client, &deadline));
    now_ms = 6500;
    assert(av_wayland_timeout(&client, 0) == 0 && deliveries == 1);
    puts("AV Wayland repeat: suppressed-at-enter keys and clock rollback never repeat passed");
}
static uint32_t live_random_state = 0x5eed1234u;
static uint32_t live_random(void) {
    live_random_state ^= live_random_state << 13;
    live_random_state ^= live_random_state >> 17;
    live_random_state ^= live_random_state << 5;
    return live_random_state;
}
static void live_randomized(void) {
    static const uint32_t Keys[] = {30, 31, 42, 58, 97, 32};
    av_wayland_t client; video_window_t *window;
    live_ready(&client, &window);
    now_ms = 10000;
    unsigned fired = 0;
    /* Reference model of the host repeat rules, independent of the reducer code. */
    int focused = 0, enabled = 0, held[128] = {0};
    uint32_t candidate = 0, rate = 0;
    uint64_t delay_ns = 0, deadline_ns = 0, period_ns = 0, model_now_ns = now_ms * 1000000ull;
    for (unsigned step = 0; step < 2000; ++step) {
        if (deliveries > 900) deliveries = 0;
        unsigned before = deliveries;
        uint32_t expected_flags[4], expected_key[4];
        unsigned expected = 0;
        int advanced = 0;
        uint32_t op = live_random() % 100;
        /* Mix: downs 25%, ups 15%, focus enter 2%, leave 2%, repeat_info 1%, time advance 55%. */
        if (op < 25) {
            /* Downs favour repeatable keys so that candidates arm often. */
            uint32_t key = live_random() % 10 < 8 ? Keys[live_random() % 3] : Keys[3 + live_random() % 3];
            if (focused) {
                /* A fresh down clears any candidate; the candidate is armed only for an enabled, repeatable key. */
                live_key(&client, key, 1);
                expected_flags[expected] = 1; expected_key[expected++] = key;
                if (!held[key]) {
                    held[key] = 1;
                    candidate = 0;
                    if (enabled && rate > 0 && av_input_key_repeatable(key)) {
                        candidate = key;
                        deadline_ns = model_now_ns + delay_ns;
                    }
                }
            } else live_key(&client, key, 1);
        } else if (op < 40) {
            uint32_t key = Keys[live_random() % 6];
            if (focused) {
                live_key(&client, key, 0);
                expected_flags[expected] = 0; expected_key[expected++] = key;
                if (held[key]) held[key] = 0;
                if (candidate == key) candidate = 0;
            } else live_key(&client, key, 0);
        } else if (op < 42) {
            if (focused) { candidate = 0; for (unsigned k = 0; k < 128; ++k) held[k] = 0; }
            live_enter(&client, window);
            focused = 1;
        } else if (op < 44) {
            if (focused) { candidate = 0; for (unsigned k = 0; k < 128; ++k) held[k] = 0; }
            keyboard_leave(&client, NULL, 0, window->surface);
            focused = 0;
        } else if (op < 45) {
            static const int32_t Rates[] = {0, 1, 3, 25, 1000, 1001, -1};
            static const int32_t Delays[] = {0, 17, 100, 60000, 60001, -1};
            int32_t new_rate = Rates[live_random() % 7], new_delay = Delays[live_random() % 6];
            keyboard_repeat(&client, NULL, new_rate, new_delay);
            if (new_rate >= 0 && new_delay >= 0 && new_rate <= 1000 && new_delay <= 60000) {
                enabled = new_rate > 0;
                rate = (uint32_t)new_rate;
                delay_ns = (uint64_t)new_delay * 1000000u;
                period_ns = rate ? (1000000000ull + rate - 1) / rate : 0;
                candidate = 0;
            }
        } else {
            uint64_t advance = live_random() % 30;
            now_ms += advance;
            model_now_ns = now_ms * 1000000ull;
            assert(av_wayland_timeout(&client, 0) == 0);
            if (candidate && model_now_ns >= deadline_ns) {
                expected_flags[expected] = 1; expected_key[expected++] = candidate;
                deadline_ns = model_now_ns + period_ns;
                ++fired;
            }
            advanced = 1;
        }
        /* Every key event of this step matches the model, in order. */
        unsigned keys = 0;
        for (unsigned i = before; i < deliveries; ++i) {
            if (delivered_log[i].type != MsgInputKey) continue;
            assert(keys < expected);
            assert(delivered_log[i].flags == expected_flags[keys] && delivered_log[i].width == expected_key[keys]);
            ++keys;
        }
        assert(keys == expected);
        /* After each poll, the timeout matches the model: the next deadline rounded up to whole ms, capped. */
        if (advanced) {
            int timeout = av_wayland_timeout(&client, 1000);
            int want = 1000;
            if (candidate) {
                uint64_t remaining = deadline_ns > model_now_ns ? deadline_ns - model_now_ns : 0;
                uint64_t ms = (remaining + 999999) / 1000000;
                if (ms < 1000) want = (int)ms;
            }
            assert(timeout == want);
        }
    }
    assert(fired >= 20); /* many separate arms fired; every step is still compared exactly */
    puts("AV Wayland repeat randomized: 2000-step lifecycle matches the reference model passed");
}
/* Pointer axes and buttons (IMPL_DESC 3.6.4). Each frame is closed with wl_pointer.frame. */
static void pointer_live_ready(av_wayland_t *client, video_window_t **window) {
    live_ready(client, window);
    live_enter(client, *window);
    pointer_enter(client, NULL, 0, (*window)->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
    deliveries = 0;
}
static void wheel_frame(av_wayland_t *client, uint32_t axis, wl_fixed_t value) {
    pointer_axis(client, NULL, 0, axis, value);
    pointer_frame(client, NULL);
}
static void pointer_buttons_live(void) {
    av_wayland_t client; video_window_t *window;
    pointer_live_ready(&client, &window);
    pointer_button(&client, NULL, 0, 0, 0x113, 1); /* BTN_SIDE -> XBUTTON1 (4) */
    assert(deliveries == 1 && delivered.type == MsgInputButton && delivered.width == 4 && delivered.flags == 1);
    pointer_button(&client, NULL, 0, 0, 0x113, 0);
    assert(deliveries == 2 && delivered.width == 4 && delivered.flags == 0);
    pointer_button(&client, NULL, 0, 0, 0x114, 1); /* BTN_EXTRA -> XBUTTON2 (5) */
    assert(deliveries == 3 && delivered.width == 5 && delivered.flags == 1);
    pointer_button(&client, NULL, 0, 0, 0x115, 1); /* BTN_FORWARD is dropped */
    pointer_button(&client, NULL, 0, 0, 0x116, 1); /* BTN_BACK is dropped */
    assert(deliveries == 3);
    /* A held XBUTTON is released with the other buttons when the pointer leaves. */
    pointer_leave(&client, NULL, 0, window->surface);
    assert(deliveries == 4 && delivered.type == MsgInputRelease && delivered.flags == AvInputReleaseButtons);
    puts("AV Wayland pointer buttons: BTN_SIDE/BTN_EXTRA map to widths 4/5, BTN_FORWARD/BTN_BACK dropped, leave releases passed");
}
static void wheel_paths_live(void) {
    av_wayland_t client; video_window_t *window;
    pointer_live_ready(&client, &window);
    /* wl_pointer v5: axis_discrete notches * 120. */
    client.seat_version = 5;
    pointer_discrete(&client, NULL, 0, 2);
    pointer_frame(&client, NULL);
    assert(deliveries == 1 && delivered.type == MsgInputWheel && delivered.y == -240 && delivered.x == 0);
    pointer_discrete(&client, NULL, 1, -1);
    pointer_frame(&client, NULL);
    assert(deliveries == 2 && delivered.x == -120 && delivered.y == 0);
    /* Continuous 10 px is 120 units; a frame with two events sums before conversion. */
    wheel_frame(&client, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    assert(deliveries == 3 && delivered.y == -120);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_frame(&client, NULL);
    assert(deliveries == 4 && delivered.y == -240);
    /* wl_pointer v8: axis_value120 is used, and it takes precedence over the other sources in the frame. */
    client.seat_version = 8;
    pointer_axis_value120(&client, NULL, 0, 120);
    pointer_discrete(&client, NULL, 0, 9);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(100));
    pointer_frame(&client, NULL);
    assert(deliveries == 5 && delivered.y == -120);
    /* Natural scrolling (v9 inverted) negates the logical value before the Windows sign rule. */
    pointer_axis_relative_direction(&client, NULL, 0, WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED);
    pointer_axis_value120(&client, NULL, 0, 120);
    pointer_frame(&client, NULL);
    assert(deliveries == 6 && delivered.y == 120);
    pointer_axis_relative_direction(&client, NULL, 0, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    /* Horizontal keeps the Wayland sign: right is positive. One frame, both axes, one message. */
    pointer_axis_value120(&client, NULL, 1, -240);
    pointer_axis_value120(&client, NULL, 0, 120);
    pointer_frame(&client, NULL);
    assert(deliveries == 7 && delivered.x == -240 && delivered.y == -120);
    /* A frame without axis events sends nothing. */
    pointer_frame(&client, NULL);
    assert(deliveries == 7);
    /* A frame larger than one message is split into ordered 1200-unit messages. */
    pointer_axis_value120(&client, NULL, 0, 2500);
    pointer_frame(&client, NULL);
    assert(deliveries == 10 && delivered_log[7].y == -1200 && delivered_log[8].y == -1200 && delivered_log[9].y == -100);
    /* Continuous remainder carries across frames; 85/256 px is 1020/256 units per frame. */
    client.seat_version = 9;
    deliveries = 0;
    wheel_frame(&client, WL_POINTER_AXIS_VERTICAL_SCROLL, 85);
    assert(deliveries == 1 && delivered.y == -3);
    wheel_frame(&client, WL_POINTER_AXIS_VERTICAL_SCROLL, 85);
    assert(deliveries == 2 && delivered.y == -4);
    wheel_frame(&client, WL_POINTER_AXIS_VERTICAL_SCROLL, 85);
    assert(deliveries == 3 && delivered.y == -4 && client.axis_carry[0] == 244);
    /* Leaving the surface resets the remainder: the next frame starts from zero. */
    pointer_leave(&client, NULL, 0, window->surface);
    pointer_enter(&client, NULL, 0, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
    deliveries = 0;
    assert(client.axis_carry[0] == 0);
    wheel_frame(&client, WL_POINTER_AXIS_VERTICAL_SCROLL, 85);
    assert(deliveries == 1 && delivered.y == -3);
    /* Bind at min(advertised, 9): a v10 advertisement binds v9, a v4 advertisement is refused. */
    av_wayland_t bind; memset(&bind, 0, sizeof(bind));
    bind.registry = (struct wl_registry *)(uintptr_t)1;
    registry_global(&bind, bind.registry, 7, "wl_seat", 10);
    assert(bind.seat_version == 9 && bind.seat != NULL);
    av_wayland_t refused; memset(&refused, 0, sizeof(refused));
    refused.registry = (struct wl_registry *)(uintptr_t)1;
    registry_global(&refused, refused.registry, 7, "wl_seat", 4);
    assert(refused.seat == NULL && refused.seat_version == 0);
    puts("AV Wayland wheel: v5 discrete, v8 value120 precedence, natural scrolling, sign, frame sums, chunks, carry and reset passed");
}
/* Randomized pointer lifecycle: compositor-side held buttons and axis frames against a guest model
 * built only from delivered events. Focus loss, pointer leave and held XBUTTONs are included. */
static void pointer_randomized_live(void) {
    static const uint32_t Buttons[] = {0x110, 0x111, 0x112, 0x113, 0x114, 0x115};
    av_wayland_t client; video_window_t *window;
    pointer_live_ready(&client, &window);
    int cheld[6] = {0}, gheld[6] = {0}, pointer_in = 1, focused = 1;
    unsigned checked = 0;
    for (unsigned step = 0; step < 1500; ++step) {
        if (deliveries > 900) deliveries = 0;
        unsigned before = deliveries;
        uint32_t op = live_random() % 100;
        int clears_guest = 0;
        if (op < 35) {
            unsigned index = live_random() % 6;
            if (!cheld[index]) {
                cheld[index] = 1;
                pointer_button(&client, NULL, 0, 0, Buttons[index], 1);
            } else {
                cheld[index] = 0;
                pointer_button(&client, NULL, 0, 0, Buttons[index], 0);
            }
        } else if (op < 55) {
            uint32_t axis = live_random() % 2;
            int32_t units = (int32_t)(live_random() % 401) - 200;
            if (live_random() % 2) pointer_axis_value120(&client, NULL, axis, units);
            else pointer_discrete(&client, NULL, axis, units / 120);
            if (live_random() % 3 == 0) pointer_frame(&client, NULL);
        } else if (op < 70) {
            pointer_frame(&client, NULL);
        } else if (op < 78) {
            /* Pointer leaves: every held button is released for the guest. */
            pointer_leave(&client, NULL, 0, window->surface);
            pointer_in = 0;
            for (unsigned i = 0; i < 6; ++i) cheld[i] = 0; /* the compositor drops its grab */
            clears_guest = 1;
        } else if (op < 86) {
            if (!pointer_in) {
                pointer_enter(&client, NULL, 0, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
                pointer_in = 1;
            }
        } else if (op < 92) {
            /* Keyboard focus loss: the guest focus clear releases everything. */
            keyboard_leave(&client, NULL, 0, window->surface);
            focused = 0;
            for (unsigned i = 0; i < 6; ++i) cheld[i] = 0;
            clears_guest = 1;
        } else {
            if (!focused) {
                live_enter(&client, window);
                focused = 1;
                pointer_in = 1;
                pointer_enter(&client, NULL, 0, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
            }
        }
        /* Guest model: apply exactly the delivered events in order. */
        for (unsigned i = before; i < deliveries; ++i) {
            const av_message_t *event = &delivered_log[i];
            if (event->type == MsgInputButton) {
                assert(event->width >= 1 && event->width <= 5);
                /* Wire buttons 1..5 map to indices 0..4. A down must never duplicate a held button;
                 * an up for a button the guest does not hold is delivered and ignored by the guest. */
                unsigned index = event->width - 1;
                if (event->flags) assert(!gheld[index]);
                gheld[index] = event->flags != 0;
            } else if (event->type == MsgInputRelease && (event->flags & AvInputReleaseButtons)) {
                for (unsigned k = 0; k < 6; ++k) gheld[k] = 0;
            } else if (event->type == MsgInputFocus && event->flags == 0) {
                for (unsigned k = 0; k < 6; ++k) gheld[k] = 0;
            }
        }
        if (clears_guest) {
            for (unsigned k = 0; k < 6; ++k) assert(!gheld[k]); /* no stuck XBUTTON or button after loss */
        }
        ++checked;
    }
    /* Final release: leaving the surface and losing focus leave nothing held on the guest. */
    pointer_leave(&client, NULL, 0, window->surface);
    keyboard_leave(&client, NULL, 0, window->surface);
    for (unsigned k = 0; k < 6; ++k) assert(!gheld[k]);
    assert(checked == 1500);
    puts("AV Wayland pointer randomized: 1500-step button, axis, leave and focus-loss lifecycle has no stuck buttons passed");
}
static void live_repeat(void) {
    live_repeat_timing();
    live_repeat_info();
    live_repeat_cancel();
    live_repeat_suppressed_and_rollback();
    live_randomized();
}
int main(void) {
    av_wayland_t client = {.request = receive_request, .request_context = &deliveries};
    client.windows[0] = (video_window_t){.client = &client, .geometry = {.window_id = 1}, .incarnation = 10};
    uint8_t mapping[64] = {0};
    assert(av_wayland_watch(&client, 0, 1, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 0, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 0x1000000, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, NULL, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), NULL, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), done, NULL) == 0);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), done, NULL) == -1);
    video_window_t window = {.client = &client, .incarnation = 10, .geometry = {.window_id = 1,
        .width = 320, .height = 200, .dpi = 96, .x = 10, .y = 20}};
    uint32_t full = XDG_TOPLEVEL_STATE_FULLSCREEN;
    struct wl_array states = {.size = sizeof(full), .alloc = sizeof(full), .data = &full};
    toplevel_configure(&window, NULL, 0, 0, &states);
    assert(deliveries == 1 && delivered.type == MsgWindowGeometry);
    assert(delivered.width == 320 && delivered.height == 200);
    assert(delivered.x == 10 && delivered.y == 20 && delivered.flags == AvWindowFullscreen);
    window.geometry = delivered;
    toplevel_configure(&window, NULL, 0, 0, &states);
    assert(deliveries == 1); /* An unchanged state does not echo a request. */
    states.size = 0;
    toplevel_configure(&window, NULL, 0, 0, &states);
    assert(deliveries == 2 && delivered.flags == 0);
    window.geometry = delivered;
    toplevel_configure(&window, NULL, 800, 600, &states);
    assert(deliveries == 3 && delivered.width == 800 && delivered.height == 600);
    window.geometry = delivered;
    toplevel_configure(&window, NULL, -1, 100000, &states);
    assert(deliveries == 3); /* Invalid dimensions cannot escape into guest requests. */
    toplevel_configure(&window, NULL, 0, 700, &states);
    assert(deliveries == 4 && delivered.width == 800 && delivered.height == 700);
    window.geometry = delivered;
    toplevel_configure(&window, NULL, 900, 0, &states);
    assert(deliveries == 5 && delivered.width == 900 && delivered.height == 700);
    window.geometry = delivered;
    states.size = sizeof(full);
    fail_delivery = 1;
    toplevel_configure(&window, NULL, 0, 0, &states);
    assert(deliveries == 6 && client.delivery_failed);
    input_callbacks();
    live_repeat();
    pointer_buttons_live();
    wheel_paths_live();
    pointer_randomized_live();
    lifecycle_callbacks();
    decoration_callbacks();
    lease_callbacks();
    lease_failures();
    lease_lifecycle();
    return 0;
}

static void lifecycle_callbacks(void) {
    deliveries = 0; fail_delivery = 0; native_effects = 0;
    av_wayland_t client = {.request = receive_request, .request_context = &deliveries};
    window_slot_header_t old_slots[3] = {0}, new_slots[3] = {0}, other_slots[3] = {0};
    window_slot_header_t *old_ptrs[3] = {&old_slots[0], &old_slots[1], &old_slots[2]};
    window_slot_header_t *new_ptrs[3] = {&new_slots[0], &new_slots[1], &new_slots[2]};
    window_slot_header_t *other_ptrs[3] = {&other_slots[0], &other_slots[1], &other_slots[2]};
    uint64_t offsets[3] = {0, 4096, 8192};
    av_message_t create = {.type = MsgWindowCreateV2, .window_id = 7, .sequence = 10,
        .process_id = 100, .width = 16, .height = 16, .dpi = 96};
    assert(av_wayland_create(&client, &create, 1, 16384, old_ptrs, offsets, 4096) == 0);
    video_window_t *old = find_window(&client, 7);
    assert(old && old->incarnation == 10 && old->pool_index == 0);
    unsigned before = native_effects;
    assert(av_wayland_create(&client, &create, 1, 16384, old_ptrs, offsets, 4096) == -1);
    assert(native_effects == before); /* duplicate live ID never allocates */
    old->buffers[0].busy = 1; atomic_store(&old_slots[0].slot_state, SlotConsuming);
    client.keyboard_window = old; client.pointer_window = old;
    av_message_t destroy = {.type = MsgWindowDestroy, .window_id = 7, .sequence = 10};
    assert(av_wayland_message(&client, &destroy) == 0 && old->retired);
    assert(!client.keyboard_window && !client.pointer_window && old->buffers[0].busy);
    create.sequence = 20;
    before = native_effects;
    assert(av_wayland_create(&client, &create, 1, 16384, old_ptrs, offsets, 4096) == -1);
    assert(native_effects == before); /* old busy pool cannot alias replacement */
    create.buffer_index = 1;
    assert(av_wayland_create(&client, &create, 1, 16384, new_ptrs, offsets, 4096) == 0);
    video_window_t *replacement = find_window(&client, 7);
    assert(replacement && replacement != old && replacement->incarnation == 20);
    client.keyboard_window = replacement; client.pointer_window = replacement;
    av_message_t saved = replacement->geometry;
    before = native_effects;
    for (unsigned flags = 0; flags <= 3; ++flags) {
        av_message_t late = saved; late.type = MsgWindowGeometry; late.sequence = 10;
        late.width = 800; late.flags = flags;
        assert(av_wayland_message(&client, &late) == 0);
        assert(av_wayland_message(&client, &destroy) == 0);
        assert(memcmp(&replacement->geometry, &saved, sizeof(saved)) == 0);
        assert(client.keyboard_window == replacement && client.pointer_window == replacement);
        assert(native_effects == before && old->buffers[0].busy);
        assert(atomic_load(&old_slots[0].slot_state) == SlotConsuming);
    }
    destroy.sequence = 0; destroy.window_id = 99;
    assert(av_wayland_message(&client, &destroy) == -1 && native_effects == before);
    destroy.sequence = 30;
    assert(av_wayland_message(&client, &destroy) == 0 && native_effects == before);
    create.window_id = 8; create.sequence = 30; create.buffer_index = 2;
    assert(av_wayland_create(&client, &create, 1, 16384, other_ptrs, offsets, 4096) == 0);
    video_window_t *other = find_window(&client, 8);
    assert(other && client.identity.high_water == 30);
    av_message_t current = saved; current.type = MsgWindowGeometry; current.flags = AvWindowFullscreen;
    assert(av_wayland_message(&client, &current) == 0 && native_effects > before);
    assert(replacement->incarnation == 20 && replacement->pool_index == 1);
    current.process_id = 0;
    current.flags = AvWindowMinimized;
    assert(av_wayland_message(&client, &current) == 0);
    assert(replacement->geometry.process_id == 100);
    current.process_id = 100;
    current.flags = 0; current.buffer_index = 0;
    assert(av_wayland_message(&client, &current) == 0 && replacement->pool_index == 1);
    before = native_effects; current.process_id = 999; current.flags = AvWindowFullscreen;
    assert(av_wayland_message(&client, &current) == 0 && native_effects == before);
    assert(replacement->geometry.process_id == 100 && !replacement->geometry.flags);
    toplevel_close(replacement, NULL);
    assert(deliveries == 1 && delivered.type == MsgWindowClose && delivered.sequence == 20);
    uint32_t fullscreen = XDG_TOPLEVEL_STATE_FULLSCREEN;
    struct wl_array states = {.size = sizeof(fullscreen), .data = &fullscreen};
    toplevel_configure(replacement, NULL, 800, 600, &states);
    assert(deliveries == 2 && delivered.sequence == 20);
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned sent = deliveries;
        video_window_t *target = mode == 0 ? old : replacement;
        if (mode == 1) client.shutting_down = 1;
        if (mode == 2) client.delivery_failed = 1;
        if (mode == 3) replacement->incarnation = 0;
        toplevel_close(target, NULL); toplevel_configure(target, NULL, 900, 700, &states);
        assert(deliveries == sent);
        client.shutting_down = client.delivery_failed = 0;
    }
    replacement->incarnation = 20;
    before = native_effects;
    surface_configure(old, NULL, 1);
    assert(native_effects == before && !old->configured);
    buffer_release(&old->buffers[0], NULL);
    assert(!old->buffers[0].busy && atomic_load(&old_slots[0].slot_state) == SlotFree);
    assert(client.keyboard_window == replacement && !replacement->retired);
    destroy.window_id = 7; destroy.sequence = 20;
    assert(av_wayland_message(&client, &destroy) == 0 && replacement->retired);
    assert(!client.keyboard_window && !client.pointer_window && !other->retired);
    free_window(old); free_window(replacement); free_window(other);
    /* Both old guest generations remain display-only even after Geometry zero. */
    for (unsigned nonzero = 0; nonzero < 2; ++nonzero) {
        client = (av_wayland_t){.request = receive_request, .request_context = &deliveries};
        create.type = MsgWindowCreate; create.window_id = 7; create.sequence = nonzero ? 99 : 0; create.buffer_index = 0;
        assert(av_wayland_create(&client, &create, 1, 16384, old_ptrs, offsets, 4096) == 0);
        video_window_t *legacy = find_window(&client, 7);
        assert(legacy && !legacy->incarnation && client.identity.mode == AvIdentityLegacy);
        av_message_t geometry = create; geometry.type = MsgWindowGeometry; geometry.sequence = 0;
        assert(av_wayland_message(&client, &geometry) == 0);
        unsigned sent = deliveries;
        toplevel_close(legacy, NULL); toplevel_configure(legacy, NULL, 800, 600, &states);
        send_input(&client, legacy, MsgInputFocus, 1, 0, 0, 0);
        assert(deliveries == sent);
        destroy.sequence = 0;
        assert(av_wayland_message(&client, &destroy) == 0);
        free_window(legacy);
    }
    /* Async import callbacks after retirement/failure/shutdown drain only the
     * original unpresented lease and cannot attach a buffer or mutate input size. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        client = (av_wayland_t){.request = receive_request, .request_context = &deliveries};
        video_window_t pending = {.client = &client};
        video_buffer_t imported = {.window = &pending, .slot = &old_slots[0], .busy = 1};
        if (mode == 0) pending.retired = 1;
        if (mode == 1) client.shutting_down = 1;
        if (mode == 2) client.delivery_failed = 1;
        atomic_store(&old_slots[0].slot_state, SlotConsuming);
        before = native_effects;
        imported_buffer(&imported, NULL, NULL);
        assert(native_effects == before + 2 && !imported.busy && !imported.buffer);
        assert(atomic_load(&old_slots[0].slot_state) == SlotFree && !pending.input_width);
        imported.busy = 1; atomic_store(&old_slots[0].slot_state, SlotConsuming);
        before = native_effects;
        rejected_buffer(&imported, NULL);
        assert(native_effects == before + 1 && !imported.busy && !pending.input_width);
        assert(atomic_load(&old_slots[0].slot_state) == SlotFree);
        before = native_effects; surface_configure(&pending, NULL, 1);
        assert(native_effects == before && !pending.configured);
    }
    puts("AV Wayland identity: same-HWND stale lifecycle, busy pools, focus and callback guards passed");
}

static void lease_init(av_wayland_t *client) {
    deliveries = 0; fail_delivery = 0; fail_delivery_at = 0;
    clock_failure = sync_failure = listener_failure = flush_error = 0;
    now_ms = 1000;
    *client = (av_wayland_t){.request = receive_request, .request_context = &deliveries,
        .identity = {.mode = AvIdentityLease}, .keymap_ready = 1,
        .display = (struct wl_display *)(uintptr_t)1};
    for (unsigned i = 0; i < 2; ++i)
        client->windows[i] = (video_window_t){.client = client,
            .geometry = {.window_id = i + 1, .sequence = (i + 1) * 10, .process_id = 100,
                .width = 320, .height = 200, .dpi = 96}, .incarnation = (i + 1) * 10,
            .surface = (struct wl_surface *)(uintptr_t)(i + 1), .input_width = 320, .input_height = 200};
}
static void fire_callback(struct wl_callback *callback, uint32_t callback_data) {
    unsigned index = proxy_index((struct wl_proxy *)callback);
    assert(callback_listeners[index] && callback_listeners[index]->done);
    callback_listeners[index]->done(callback_contexts[index], callback, callback_data);
}
static void begin_lease(av_wayland_t *client, uint64_t epoch) {
    av_message_t revoked = {.type = MsgInputEpochRevoked, .lease_generation = epoch};
    unsigned before = flushes;
    assert(av_wayland_message(client, &revoked) == 0);
    assert(client->lease.phase == AvLeaseBarrier && client->lease_sync && flushes == before + 1);
    assert(client->lease_sync_epoch == epoch && client->lease.deadline == now_ms + AvLeaseTimeoutMs);
    assert(!client->keyboard_window && !client->pointer_window);
}
static void finish_lease(av_wayland_t *client) {
    uint64_t deadline = client->lease.deadline;
    struct wl_callback *callback = client->lease_sync;
    unsigned before = deliveries;
    fire_callback(callback, UINT32_MAX);
    assert(deliveries == before + 1 && delivered.type == MsgInputEpochAck);
    assert(client->lease.phase == AvLeaseAwaitReady && client->lease.deadline == deadline);
    assert(!client->lease_sync && !client->lease_sync_epoch);
    assert(destroyed_proxies[proxy_index((struct wl_proxy *)callback)]);
    av_message_t ready = {.type = MsgInputEpochReady, .lease_generation = client->lease.epoch,
                          .buffer_index = client->lease.ack_serial};
    assert(av_wayland_message(client, &ready) == 0);
    assert(client->lease.phase == AvLeaseHostUnfocusedReady && !client->lease.deadline);
    assert(!client->keyboard_window && !client->pointer_window && deliveries == before + 1);
}
static void enter_empty(av_wayland_t *client, unsigned window) {
    struct wl_array keys = {0};
    keyboard_enter(client, NULL, 0, client->windows[window].surface, &keys);
}
static void lease_callbacks(void) {
    av_wayland_t client;
    lease_init(&client);
    video_window_t *first = &client.windows[0], *second = &client.windows[1];
    struct wl_array empty = {0};
    uint32_t held = 30;
    struct wl_array keys = {.size = sizeof(held), .data = &held};
    keyboard_key(&client, NULL, 0, 0, 30, 1);
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    pointer_enter(&client, NULL, 0, first->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(!deliveries && !client.keyboard_window && !client.pointer_window);
    assert(client.observed_keys[30] && client.suppressed_keys[30]);
    assert(client.observed_buttons[1] && client.suppressed_buttons[1]);
    client.lease.serial = 100; /* Lease controls must never index video buffers. */
    client.watch_sync = wl_display_sync(client.display);
    struct wl_callback *watch = client.watch_sync;
    begin_lease(&client, 1);
    struct wl_callback *barrier = client.lease_sync;
    assert(barrier != watch && client.watch_sync == watch);
    unsigned destroys = proxy_destroys;
    lease_done(&client, watch, 1);
    assert(!deliveries && client.lease_sync == barrier && proxy_destroys == destroys);
    held = 42;
    keyboard_enter(&client, NULL, 0, NULL, &keys); /* Unknown surface is still authoritative. */
    assert(!client.observed_keys[30] && !client.suppressed_keys[30]);
    assert(client.observed_keys[42] && client.suppressed_keys[42]);
    keyboard_key(&client, NULL, 0, 0, 31, 1);
    keyboard_key(&client, NULL, 0, 0, 31, 0);
    pointer_button(&client, NULL, 0, 0, 0x110, 0);
    assert(!client.observed_keys[31] && !client.suppressed_keys[31]);
    assert(!client.observed_buttons[1] && !client.suppressed_buttons[1]);
    uint64_t deadline = client.lease.deadline;
    now_ms = 1500;
    assert(av_wayland_timeout(&client, 2000) == 1500);
    fire_callback(barrier, 0); /* callback_data carries no lease identity. */
    assert(deliveries == 1 && delivered.type == MsgInputEpochAck && delivered.buffer_index == 101);
    assert(delivered.lease_generation == 1 && client.lease.deadline == deadline);
    fire_callback(barrier, 1); /* Saved late callback cannot Ack twice. */
    assert(deliveries == 1);
    pointer_enter(&client, NULL, 0, first->surface, wl_fixed_from_int(12), wl_fixed_from_int(21));
    keyboard_enter(&client, NULL, 0, first->surface, &empty);
    assert(!client.keyboard_window && !client.pointer_window && deliveries == 1);
    av_message_t ready = {.type = MsgInputEpochReady, .lease_generation = 1, .buffer_index = 101};
    assert(av_wayland_message(&client, &ready) == 0);
    assert(client.lease.phase == AvLeaseHostUnfocusedReady && !client.lease.deadline && deliveries == 1);
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(15), wl_fixed_from_int(22));
    assert(!client.pointer_window && deliveries == 1); /* Motion never restores identity. */
    keyboard_key(&client, NULL, 0, 0, 30, 1);
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    held = 30;
    pointer_enter(&client, NULL, 0, first->surface, wl_fixed_from_int(16), wl_fixed_from_int(23));
    assert(client.pointer_window == first && deliveries == 1);
    keyboard_enter(&client, NULL, 0, first->surface, &keys);
    assert(client.lease.phase == AvLeaseForwarding && client.keyboard_window == first && deliveries == 3);
    assert(delivered_log[1].type == MsgInputFocusV3 && delivered_log[1].flags == 1);
    assert(delivered_log[2].type == MsgInputPointerV3 && delivered_log[2].x == 16 && delivered_log[2].y == 23);
    keyboard_key(&client, NULL, 0, 0, 30, 1);
    keyboard_key(&client, NULL, 0, 0, 30, 0);
    assert(deliveries == 3 && !client.suppressed_keys[30]);
    keyboard_key(&client, NULL, 0, 0, 30, 1);
    keyboard_key(&client, NULL, 0, 0, 30, 0);
    assert(deliveries == 5 && delivered.type == MsgInputKeyV3 && !delivered.flags);
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    pointer_button(&client, NULL, 0, 0, 0x110, 0);
    assert(deliveries == 5); /* No invented button snapshot at keyboard/pointer enter. */
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == 7 && delivered_log[5].type == MsgInputPointerV3 && delivered.type == MsgInputButtonV3);
    pointer_motion(&client, NULL, 0, -1, wl_fixed_from_int(23));
    pointer_button(&client, NULL, 0, 0, 0x110, 0);
    assert(deliveries == 8 && delivered.type == MsgInputButtonV3 && !delivered.flags);
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(19), wl_fixed_from_int(24));
    unsigned before = deliveries;
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_frame(&client, NULL);
    assert(deliveries == before + 2 && delivered_log[before].type == MsgInputPointerV3);
    assert(delivered_log[before].x == 19 && delivered.type == MsgInputWheelV3 && delivered.y == -120);
    keyboard_key(&client, NULL, 0, 0, 31, 1);
    pointer_button(&client, NULL, 0, 0, 0x111, 1);
    pointer_leave(&client, NULL, 0, first->surface);
    assert(delivered.type == MsgInputReleaseV3 && delivered.flags == AvInputReleaseButtons);
    assert(client.observed_buttons[2] && client.suppressed_buttons[2]);
    pointer_enter(&client, NULL, 0, second->surface, wl_fixed_from_int(27), wl_fixed_from_int(28));
    before = deliveries;
    held = 31;
    keyboard_enter(&client, NULL, 0, second->surface, &keys);
    assert(deliveries == before + 3 && client.keyboard_window == second);
    assert(delivered_log[before].type == MsgInputFocusV3 && !delivered_log[before].flags && delivered_log[before].window_id == 1);
    assert(delivered_log[before + 1].type == MsgInputFocusV3 && delivered_log[before + 1].flags && delivered_log[before + 1].window_id == 2);
    assert(delivered_log[before + 2].type == MsgInputPointerV3 && delivered_log[before + 2].x == 27);
    assert(client.suppressed_keys[31] && client.suppressed_buttons[2]);
    begin_lease(&client, 2);
    assert(client.suppressed_keys[31] && client.suppressed_buttons[2]);
    before = deliveries;
    assert(av_wayland_message(&client, &ready) == 0 && deliveries == before); /* Canonical old Ready is inert. */
    assert(client.lease.phase == AvLeaseBarrier);
    keyboard_key(&client, NULL, 0, 0, 31, 0);
    keyboard_key(&client, NULL, 0, 0, 32, 1);
    pointer_button(&client, NULL, 0, 0, 0x111, 0);
    assert(!client.suppressed_keys[31] && client.suppressed_keys[32] && !client.suppressed_buttons[2]);
    finish_lease(&client);
    before = deliveries;
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(32), wl_fixed_from_int(33));
    assert(!client.pointer_window && deliveries == before);
    enter_empty(&client, 1);
    assert(deliveries == before + 1 && delivered.type == MsgInputFocusV3);
    assert(!client.observed_keys[32] && !client.suppressed_keys[32]);
    pointer_enter(&client, NULL, 0, second->surface, wl_fixed_from_int(32), wl_fixed_from_int(33));
    pointer_button(&client, NULL, 0, 0, 0x111, 1);
    assert(delivered.type == MsgInputButtonV3 && delivered.flags);
    for (unsigned i = 1; i < deliveries; ++i)
        assert(delivered_log[i].buffer_index > delivered_log[i - 1].buffer_index);
    keyboard_keymap(&client, NULL, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, -1, 0);
    assert(delivered.type == MsgInputFocusV3 && !delivered.flags && !client.keyboard_window);
    assert(client.lease.phase == AvLeaseHostUnfocusedReady && client.suppressed_buttons[2]);
    wl_callback_destroy(client.watch_sync);
    client.watch_sync = NULL;
    puts("AV Wayland leases: ordered barrier, fresh focus, authoritative snapshots and pointer synchronization passed");
}

static av_wayland_t *draining_client;
static struct wl_callback *draining_callback;
static void late_drain_callbacks(void) {
    during_drain = NULL;
    assert(draining_client->shutting_down && draining_client->lease.phase == AvLeaseHostTerminal);
    assert(!draining_client->lease_sync && !draining_client->keyboard_window && !draining_client->pointer_window);
    unsigned before = deliveries;
    fire_callback(draining_callback, 1);
    enter_empty(draining_client, 0);
    keyboard_key(draining_client, NULL, 0, 0, 30, 1);
    pointer_enter(draining_client, NULL, 0, draining_client->windows[0].surface, 0, 0);
    pointer_button(draining_client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == before && !draining_client->lease_sync && !draining_client->keyboard_window);
}
static void lease_failures(void) {
    av_wayland_t client;
    av_message_t revoked = {.type = MsgInputEpochRevoked, .lease_generation = 1};
    for (unsigned mode = AvIdentityUnknown; mode <= AvIdentityModern; ++mode) {
        lease_init(&client);
        client.identity.mode = (av_identity_mode_t)mode;
        unsigned before = native_effects;
        assert(av_wayland_message(&client, &revoked) == -1 && !client.lease_sync);
        assert(native_effects == before && !deliveries);
    }
    lease_init(&client);
    av_message_t old_input = {.type = MsgInputKey, .window_id = 1, .sequence = 10,
        .buffer_index = 1, .width = 30, .flags = 1};
    assert(av_wayland_message(&client, &old_input) == -1 && client.delivery_failed);
    for (unsigned failure = 0; failure < 3; ++failure) {
        lease_init(&client);
        if (failure == 0) sync_failure = 1;
        if (failure == 1) listener_failure = 1;
        if (failure == 2) flush_error = EIO;
        assert(av_wayland_message(&client, &revoked) == -1);
        assert(client.delivery_failed && client.lease.phase == AvLeaseHostTerminal && !client.lease_sync);
        assert(!client.lease.deadline && !deliveries);
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        lease_init(&client);
        begin_lease(&client, 1);
        struct wl_callback *callback = client.lease_sync;
        if (failure == 0) client.lease_sync_epoch = 2;
        if (failure == 1) client.lease.phase = AvLeaseAwaitReady;
        if (failure == 2) clock_failure = 1;
        if (failure == 3) now_ms = client.lease.deadline;
        fire_callback(callback, 1);
        assert(client.delivery_failed && client.lease.phase == AvLeaseHostTerminal);
        assert(!deliveries && !client.lease_sync && !client.lease.deadline);
    }
    lease_init(&client);
    begin_lease(&client, 1);
    struct wl_callback *pending = client.lease_sync;
    fail_delivery = 1;
    toplevel_close(&client.windows[0], NULL);
    assert(client.delivery_failed && !client.lease_sync && client.lease.phase == AvLeaseHostTerminal);
    fire_callback(pending, 1);
    assert(deliveries == 1 && delivered.type == MsgWindowClose);
    lease_init(&client);
    begin_lease(&client, 1); finish_lease(&client); enter_empty(&client, 0);
    pointer_enter(&client, NULL, 0, client.windows[0].surface, 0, 0);
    fail_delivery = 1;
    pointer_enter(&client, NULL, 0, client.windows[1].surface, 0, 0);
    assert(client.delivery_failed && !client.pointer_window && !client.keyboard_window);
    lease_init(&client);
    begin_lease(&client, 1);
    now_ms = client.lease.deadline - 1;
    fire_callback(client.lease_sync, 1);
    assert(client.lease.phase == AvLeaseAwaitReady && av_wayland_timeout(&client, 1000) == 1);
    av_message_t ready = {.type = MsgInputEpochReady, .lease_generation = 1,
                          .buffer_index = client.lease.ack_serial};
    ++now_ms;
    assert(av_wayland_message(&client, &ready) == -1 && client.delivery_failed);
    assert(deliveries == 1 && !client.lease.deadline);
    lease_init(&client);
    flush_error = EAGAIN;
    begin_lease(&client, 1);
    uint64_t deadline = client.lease.deadline;
    assert(av_wayland_writable(&client) && !client.delivery_failed);
    now_ms += 1999;
    assert(av_wayland_timeout(&client, 1000) == 1 && client.lease.deadline == deadline);
    flush_error = 0;
    assert(av_wayland_flush(&client) == 0 && !av_wayland_writable(&client));
    assert(client.lease.deadline == deadline);
    ++now_ms;
    assert(av_wayland_timeout(&client, 1000) == -1 && !client.lease_sync);
    for (unsigned failure = 0; failure < 3; ++failure) {
        lease_init(&client);
        if (failure == 0) clock_failure = 1;
        if (failure == 1) now_ms = 0;
        if (failure == 2) now_ms = UINT64_MAX - 1000;
        assert(av_wayland_message(&client, &revoked) == -1 && client.delivery_failed && !client.lease_sync);
    }
    lease_init(&client);
    begin_lease(&client, 1);
    assert(av_wayland_message(&client, &revoked) == -1 && !client.lease_sync); /* Overlapping revoke. */
    lease_init(&client);
    begin_lease(&client, 1);
    fail_delivery = 1;
    fire_callback(client.lease_sync, 0);
    assert(client.delivery_failed && client.lease.phase == AvLeaseHostTerminal && deliveries == 1);
    assert(!client.lease_sync && !client.lease.deadline);
    for (unsigned malformed = 0; malformed < 5; ++malformed) {
        lease_init(&client);
        uint32_t held[AvInputKeys + 1] = {30};
        struct wl_array keys = {.size = sizeof(uint32_t), .data = held};
        if (malformed == 1) keys.size = 1;
        if (malformed == 2) keys.size = sizeof(held);
        if (malformed == 3) keys.data = NULL;
        if (malformed == 4) keys.data = (uint8_t *)held + 1;
        keyboard_enter(&client, NULL, 0, NULL, malformed ? &keys : NULL);
        assert(client.delivery_failed && !client.keyboard_window && !deliveries);
    }
    lease_init(&client);
    uint32_t unsupported[] = {0, 84, 128, UINT32_MAX};
    struct wl_array unsupported_keys = {.size = sizeof(unsupported), .data = unsupported};
    keyboard_enter(&client, NULL, 0, NULL, &unsupported_keys);
    assert(!client.delivery_failed && !client.suppressed_keys[84] && !client.observed_keys[84]);
    /* A failure on either frame of click/wheel synchronization is terminal;
     * no subsequent user callback can retry the partially queued pair. */
    for (unsigned wheel = 0; wheel < 2; ++wheel) {
        for (unsigned failing_frame = 1; failing_frame <= 2; ++failing_frame) {
            lease_init(&client);
            begin_lease(&client, 1); finish_lease(&client);
            enter_empty(&client, 0);
            pointer_enter(&client, NULL, 0, client.windows[0].surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
            unsigned before = deliveries;
            fail_delivery_at = deliveries + failing_frame;
            if (wheel) {
                pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
                pointer_frame(&client, NULL);
            } else pointer_button(&client, NULL, 0, 0, 0x110, 1);
            assert(client.delivery_failed && client.lease.phase == AvLeaseHostTerminal);
            assert(deliveries == before + failing_frame && delivered_log[before].type == MsgInputPointerV3);
            pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
            pointer_frame(&client, NULL);
            pointer_button(&client, NULL, 0, 0, 0x110, 0);
            assert(deliveries == before + failing_frame);
        }
    }
    for (unsigned failing_frame = 1; failing_frame <= 2; ++failing_frame) {
        lease_init(&client);
        begin_lease(&client, 1); finish_lease(&client); enter_empty(&client, 0);
        unsigned before = deliveries;
        fail_delivery_at = deliveries + failing_frame;
        enter_empty(&client, 1);
        assert(client.delivery_failed && !client.keyboard_window && deliveries == before + failing_frame);
        assert(delivered_log[before].type == MsgInputFocusV3 && !delivered_log[before].flags);
    }
    lease_init(&client);
    begin_lease(&client, 1); finish_lease(&client);
    client.lease.serial = UINT32_MAX;
    enter_empty(&client, 0);
    assert(client.delivery_failed && !client.keyboard_window && deliveries == 1);
    av_wayland_t *owned = malloc(sizeof(*owned));
    assert(owned);
    lease_init(owned);
    begin_lease(owned, 1);
    draining_client = owned; draining_callback = owned->lease_sync;
    unsigned before_dispatch = pending_dispatches, before_disconnect = disconnects;
    during_drain = late_drain_callbacks;
    av_wayland_free(&owned);
    assert(!owned && !during_drain && !deliveries);
    assert(pending_dispatches == before_dispatch + 1 && disconnects == before_disconnect + 1);
    draining_client = NULL; draining_callback = NULL;
    puts("AV Wayland leases: malformed/late controls, callback ownership, queue/clock failure and shutdown passed");
}
static void lease_lifecycle(void) {
    av_wayland_t client;
    lease_init(&client);
    client.identity = (av_identity_session_t){0};
    memset(client.windows, 0, sizeof(client.windows));
    window_slot_header_t slots[3] = {0};
    window_slot_header_t *pointers[3] = {&slots[0], &slots[1], &slots[2]};
    uint64_t offsets[3] = {0, 4096, 8192};
    av_message_t create = {.type = MsgWindowCreateV3, .window_id = 7, .sequence = 10,
        .process_id = 100, .width = 16, .height = 16, .dpi = 96};
    assert(av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096) == 0);
    assert(client.identity.mode == AvIdentityLease && client.lease.phase == AvLeaseAwaitInitialRevocation);
    video_window_t *window = find_window(&client, 7);
    assert(window && window->incarnation == 10);
    av_message_t mixed = create; mixed.type = MsgWindowCreateV2; mixed.window_id = 8;
    mixed.sequence = 20; mixed.buffer_index = 1;
    unsigned before = native_effects;
    assert(av_wayland_create(&client, &mixed, 1, 16384, pointers, offsets, 4096) == -1);
    assert(native_effects == before);
    begin_lease(&client, 1); finish_lease(&client);
    enter_empty(&client, 0);
    keyboard_key(&client, NULL, 0, 0, 30, 1);
    client.pointer_window = window;
    client.observed_buttons[1] = 1;
    av_message_t destroy = {.type = MsgWindowDestroy, .window_id = 7, .sequence = 10};
    before = deliveries;
    assert(av_wayland_message(&client, &destroy) == 0 && window->retired);
    assert(!client.keyboard_window && !client.pointer_window && deliveries == before);
    assert(client.suppressed_keys[30] && client.suppressed_buttons[1]);
    assert(client.lease.phase == AvLeaseHostUnfocusedReady);
    create.sequence = 20;
    assert(av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096) == 0);
    window = find_window(&client, 7);
    assert(window && window->incarnation == 20 && !client.keyboard_window);
    av_message_t geometry = create; geometry.type = MsgWindowGeometry; geometry.sequence = 10;
    geometry.flags = AvWindowFullscreen; geometry.width = 64;
    before = native_effects;
    assert(av_wayland_message(&client, &geometry) == 0);
    assert(av_wayland_message(&client, &destroy) == 0);
    assert(native_effects == before && window->geometry.width == 16 && !window->retired);
    enter_empty(&client, 0);
    assert(client.keyboard_window == window && !client.suppressed_keys[30] && client.suppressed_buttons[1]);
    keyboard_key(&client, NULL, 0, 0, 31, 1);
    client.keyboard = (struct wl_keyboard *)(uintptr_t)1;
    seat_capabilities(&client, NULL, 0);
    assert(!client.keyboard_window && !client.keyboard && client.suppressed_keys[31]);
    assert(delivered.type == MsgInputFocusV3 && !delivered.flags);
    free_window(window);
    puts("AV Wayland leases: CreateV3 selection, immutable lifecycle routing and capability loss passed");
}
/* Windows-drawn frames: request client-side decoration when the compositor
 * offers xdg-decoration, re-issue titles only on change, and destroy the
 * decoration object before its toplevel as the protocol requires. */
static void decoration_callbacks(void) {
    deliveries = 0; fail_delivery = 0;
    av_wayland_t client = {.request = receive_request, .request_context = &deliveries};
    window_slot_header_t slots[3] = {0};
    window_slot_header_t *pointers[3] = {&slots[0], &slots[1], &slots[2]};
    uint64_t offsets[3] = {0, 4096, 8192};
    av_message_t create = {.type = MsgWindowCreateV2, .window_id = 9, .sequence = 30,
        .process_id = 100, .width = 16, .height = 16, .dpi = 96};
    snprintf(create.title, sizeof(create.title), "Untitled - Notepad");
    /* Without a manager no decoration object or mode request is created. */
    unsigned modes = decoration_modes, titles = title_requests;
    assert(av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096) == 0);
    video_window_t *plain = find_window(&client, 9);
    assert(plain && !plain->decoration && decoration_modes == modes);
    assert(title_requests == titles + 1 && !strcmp(last_title, "Untitled - Notepad"));
    plain->surface = NULL; plain->xdg_surface = NULL; plain->toplevel = NULL;
    free_window(plain);
    /* With a manager the toplevel is created client-side decorated. */
    proxy_names[proxy_count] = "zxdg_decoration_manager_v1";
    client.decoration_manager = (struct zxdg_decoration_manager_v1 *)&proxy_storage[proxy_count++];
    create.sequence = 31;
    assert(av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096) == 0);
    video_window_t *window = find_window(&client, 9);
    assert(window && window->decoration && decoration_modes == modes + 1);
    assert(last_decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    unsigned decoration_index = proxy_index((struct wl_proxy *)window->decoration);
    unsigned surface_index = proxy_index((struct wl_proxy *)window->xdg_surface);
    const struct zxdg_toplevel_decoration_v1_listener *listener = decoration_listeners[decoration_index];
    const struct xdg_surface_listener *surface_listener = surface_listeners[surface_index];
    assert(listener && surface_listener && decoration_contexts[decoration_index] == window);
    unsigned acks = surface_acks;
    listener->configure(window, window->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    assert(!window->configured && !window->decoration_mode && surface_acks == acks);
    surface_listener->configure(surface_contexts[surface_index], window->xdg_surface, 400);
    assert(window->configured && last_surface_serial == 400 && surface_acks == acks + 1);
    assert(window->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    assert(!window->pending_decoration_mode);
    /* A surface-only resize retains the applied mode. */
    surface_listener->configure(window, window->xdg_surface, 401);
    assert(window->decoration && surface_acks == acks + 2);
    /* Geometry with an unchanged title does not re-issue it; a change does. */
    av_message_t geometry = window->geometry;
    geometry.type = MsgWindowGeometry; geometry.width = 32;
    titles = title_requests;
    assert(av_wayland_message(&client, &geometry) == 0 && title_requests == titles);
    snprintf(geometry.title, sizeof(geometry.title), "*notes.txt - Notepad");
    assert(av_wayland_message(&client, &geometry) == 0);
    assert(title_requests == titles + 1 && !strcmp(last_title, "*notes.txt - Notepad"));
    assert(av_wayland_message(&client, &geometry) == 0 && title_requests == titles + 1);
    /* Retirement destroys the decoration before the toplevel. */
    destroy_count = 0;
    retire_window(window);
    assert(!window->decoration && !window->toplevel && destroy_count >= 2);
    assert(!strcmp(destroy_log[0], "zxdg_toplevel_decoration_v1"));
    assert(!strcmp(destroy_log[1], "xdg_toplevel"));
    unsigned effects = native_effects;
    listener->configure(window, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    surface_listener->configure(window, NULL, 402);
    assert(native_effects == effects); /* Retired callbacks cannot resurrect a surface. */
    free_window(window);
    /* A forced server-side mode is staged until its surface ack. Destroying the
     * extension then disables server decoration at the next content commit. */
    create.sequence = 32;
    assert(av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096) == 0);
    window = find_window(&client, 9);
    struct zxdg_toplevel_decoration_v1 *old_decoration = window->decoration;
    decoration_index = proxy_index((struct wl_proxy *)old_decoration);
    listener = decoration_listeners[decoration_index];
    acks = surface_acks;
    listener->configure(window, old_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    assert(window->decoration == old_decoration && !window->configured && surface_acks == acks);
    surface_configure(window, window->xdg_surface, 500);
    assert(!window->decoration && window->configured && surface_acks == acks + 1);
    assert(decoration_destroy_ack == surface_acks && last_surface_serial == 500);
    listener->configure(window, old_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    assert(!window->pending_decoration_mode); /* Queued event from destroyed object. */
    surface_configure(window, window->xdg_surface, 501);
    free_window(window);
    /* Missing initial mode, invalid modes, and listener/allocation failures fail
     * closed without leaving an admitted window or negotiated object alive. */
    for (unsigned failure = 0; failure < 4; ++failure) {
        create.sequence = 33 + failure;
        decoration_listener_failure = failure == 2;
        decoration_creation_failure = failure == 3;
        int result = av_wayland_create(&client, &create, 1, 16384, pointers, offsets, 4096);
        if (failure >= 2) {
            assert(result == -1 && !find_window(&client, 9));
        } else {
            assert(result == 0);
            window = find_window(&client, 9);
            acks = surface_acks;
            if (failure == 0) surface_configure(window, window->xdg_surface, 600);
            else decoration_configure(window, window->decoration, 99);
            assert(client.delivery_failed && !window->configured && surface_acks == acks);
            free_window(window);
            client.delivery_failed = 0;
        }
    }
    decoration_listener_failure = decoration_creation_failure = 0;
    puts("AV Wayland decorations: delivered configure, staged modes, server fallback, failures and teardown passed");
}
