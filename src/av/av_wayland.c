#include "av_wayland.h"
#include "av_dmabuf.h"
#include "av_input.h"
#include "av_identity.h"
#include "av_lease.h"
#include "linux_dmabuf_client.h"
#include "xdg_shell_client.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

/** @brief One retained compositor buffer; event thread owns protocol reference. */
typedef struct video_buffer_t {
    struct wl_buffer *buffer;
    struct zwp_linux_buffer_params_v1 *params;
    struct video_window_t *window;
    window_slot_header_t *slot;
    uint32_t width, height, stride;
    uint64_t offset;
    int busy;
} video_buffer_t;
/** @brief Retained window state, including retired windows awaiting buffer release. */
typedef struct video_window_t {
    struct av_wayland_t *client;
    av_message_t geometry;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_shm_pool *pool;
    video_buffer_t buffers[3];
    size_t capacity;
    uint64_t incarnation;
    uint32_t pool_index;
    uint32_t input_width, input_height;
    int configured, retired;
    int device_fd, dmabuf_failed;
} video_window_t;
/** @brief Event-thread context; alloc/free are the sole ownership boundary. */
struct av_wayland_t {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    int linear_argb;
    video_window_t windows[AvMaxWindows];
    av_identity_session_t identity;
    av_host_request_t request;
    void *request_context;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    video_window_t *keyboard_window, *pointer_window;
    uint32_t seat_global, input_serial;
    uint8_t suppressed_keys[AvInputKeys];
    uint8_t observed_keys[AvInputKeys], observed_buttons[4], suppressed_buttons[4];
    av_lease_host_t lease;
    struct wl_callback *lease_sync;
    uint64_t lease_sync_epoch;
    wl_fixed_t pointer_x, pointer_y;
    int keymap_ready, shutting_down;
    int delivery_failed;
    int writable;
    uint64_t watch_window, watch_incarnation;
    uint32_t watch_token;
    const uint8_t *watch_mapping;
    size_t watch_length;
    av_commit_done_t watch_done;
    void *watch_context;
    struct wl_callback *watch_sync;
};
static int controls_enabled(const video_window_t *window) {
    return window && window->incarnation && !window->retired && window->client &&
        !window->client->shutting_down && !window->client->delivery_failed;
}
static int window_active(const video_window_t *window) {
    return window && !window->retired && window->client &&
        !window->client->shutting_down && !window->client->delivery_failed;
}
static video_window_t *find_window(av_wayland_t *client, uint64_t id);
static video_window_t *input_surface(av_wayland_t *client, struct wl_surface *surface) {
    if (!surface || client->shutting_down || client->delivery_failed) return NULL;
    for (unsigned i = 0; i < AvMaxWindows; ++i) {
        video_window_t *window = &client->windows[i];
        if (window->surface == surface && controls_enabled(window))
            return window;
    }
    return NULL;
}
static uint64_t monotonic_ms(void);
static int lease_mode(const av_wayland_t *client) {
    return client->identity.mode == AvIdentityLease;
}
static int lease_ready(const av_wayland_t *client) {
    return !client->shutting_down && !client->delivery_failed &&
        (client->lease.phase == AvLeaseHostUnfocusedReady || client->lease.phase == AvLeaseForwarding);
}
static void suppress_observed(av_wayland_t *client, uint32_t mask) {
    if (!lease_mode(client)) return;
    if (mask & AvInputReleaseKeys)
        for (unsigned key = 0; key < AvInputKeys; ++key)
            client->suppressed_keys[key] |= client->observed_keys[key];
    if (mask & AvInputReleaseButtons)
        for (unsigned button = 1; button < 4; ++button)
            client->suppressed_buttons[button] |= client->observed_buttons[button];
}
static void cancel_lease_sync(av_wayland_t *client) {
    if (client->lease_sync) wl_callback_destroy(client->lease_sync);
    client->lease_sync = NULL;
    client->lease_sync_epoch = 0;
}
static void disable_routes(av_wayland_t *client) {
    suppress_observed(client, AvInputReleaseKeys | AvInputReleaseButtons);
    client->keyboard_window = NULL;
    client->pointer_window = NULL;
}
static int fail_input(av_wayland_t *client) {
    client->delivery_failed = 1;
    client->lease.phase = AvLeaseHostTerminal;
    client->lease.deadline = 0;
    disable_routes(client);
    cancel_lease_sync(client);
    return -1;
}
static void mark_delivery_failed(av_wayland_t *client) {
    if (lease_mode(client)) fail_input(client);
    else client->delivery_failed = 1;
}
static int send_input(av_wayland_t *client, video_window_t *window, uint32_t type,
                       uint32_t flags, uint32_t code, int32_t x, int32_t y) {
    if (!controls_enabled(window)) return -1;
    av_message_t event = {.type = type, .window_id = window->geometry.window_id,
        .sequence = window->incarnation, .flags = flags, .width = code, .x = x, .y = y};
    if (lease_mode(client)) {
        if (!lease_ready(client) || (type != MsgInputFocus &&
            (client->lease.phase != AvLeaseForwarding || window != client->keyboard_window))) return -1;
        if (av_lease_host_check(&client->lease, monotonic_ms()) != 0) return fail_input(client);
        event.type += MsgInputFocusV3 - MsgInputFocus;
        if (av_lease_host_stamp(&client->lease, &event) != 0) return fail_input(client);
    } else {
        if (client->input_serial == UINT32_MAX) { client->delivery_failed = 1; return -1; }
        event.buffer_index = ++client->input_serial;
    }
    if (client->request(&event, client->request_context) != 0) {
        if (lease_mode(client)) return fail_input(client);
        client->delivery_failed = 1;
        return -1;
    }
    return 0;
}
static void clear_keyboard_focus(av_wayland_t *client) {
    send_input(client, client->keyboard_window, MsgInputFocus, 0, 0, 0, 0);
    client->keyboard_window = NULL;
    if (lease_mode(client)) {
        suppress_observed(client, AvInputReleaseKeys | AvInputReleaseButtons);
        if (client->lease.phase == AvLeaseForwarding) client->lease.phase = AvLeaseHostUnfocusedReady;
    } else memset(client->suppressed_keys, 0, sizeof(client->suppressed_keys));
}
static int input_coordinates_valid(av_wayland_t *client) {
    video_window_t *window = client->pointer_window;
    if (!window || window != client->keyboard_window || client->pointer_x < 0 || client->pointer_y < 0)
        return 0;
    int32_t x = wl_fixed_to_int(client->pointer_x), y = wl_fixed_to_int(client->pointer_y);
    return (uint32_t)x < window->input_width && (uint32_t)y < window->input_height;
}
static int forward_pointer(av_wayland_t *client) {
    if (!input_coordinates_valid(client)) return -1;
    return send_input(client, client->pointer_window, MsgInputPointer, 0, 0,
                       wl_fixed_to_int(client->pointer_x), wl_fixed_to_int(client->pointer_y));
}
static void keyboard_keymap(void *context, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size) {
    (void)keyboard; (void)size;
    av_wayland_t *client = context;
    if (fd >= 0) close(fd);
    // Physical scan-code mode deliberately leaves text interpretation to guest.
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) clear_keyboard_focus(client);
    client->keymap_ready = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
}
static int keyboard_snapshot(av_wayland_t *client, const struct wl_array *keys) {
    /* Wayland owns the accessible array; validate its bounded extent and element
     * alignment before any read, including blocked or unrecognized enters. */
    if (!keys || keys->size % sizeof(uint32_t) || keys->size > AvInputKeys * sizeof(uint32_t) ||
        (keys->size && (!keys->data || (uintptr_t)keys->data % _Alignof(uint32_t))))
        return fail_input(client);
    uint8_t present[AvInputKeys] = {0};
    for (size_t offset = 0; offset < keys->size; offset += sizeof(uint32_t)) {
        uint32_t key;
        memcpy(&key, (const uint8_t *)keys->data + offset, sizeof(key));
        if (key < AvInputKeys && av_input_scan_code(key)) present[key] = 1;
    }
    memcpy(client->observed_keys, present, sizeof(present));
    memcpy(client->suppressed_keys, present, sizeof(present));
    return 0;
}
static void keyboard_enter(void *context, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard; (void)serial;
    av_wayland_t *client = context;
    if (lease_mode(client) && keyboard_snapshot(client, keys) != 0) return;
    clear_keyboard_focus(client);
    video_window_t *window = input_surface(client, surface);
    if (!window || !client->keymap_ready || (lease_mode(client) && !lease_ready(client))) return;
    if (!lease_mode(client)) {
        if (!keys || keys->size % sizeof(uint32_t) || keys->size > AvInputKeys * sizeof(uint32_t) ||
            (keys->size && !keys->data)) { client->delivery_failed = 1; return; }
        for (size_t offset = 0; offset < keys->size; offset += sizeof(uint32_t)) {
            uint32_t key;
            memcpy(&key, (const uint8_t *)keys->data + offset, sizeof(key));
            if (key < AvInputKeys) client->suppressed_keys[key] = 1;
        }
        client->keyboard_window = window;
        send_input(client, window, MsgInputFocus, 1, 0, 0, 0);
    } else {
        if (send_input(client, window, MsgInputFocus, 1, 0, 0, 0) != 0) return;
        client->keyboard_window = window;
        client->lease.phase = AvLeaseForwarding;
    }
    forward_pointer(client);
}
static void keyboard_leave(void *context, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    (void)keyboard; (void)serial;
    av_wayland_t *client = context;
    if (client->keyboard_window && client->keyboard_window->surface == surface)
        clear_keyboard_focus(client);
}
static void keyboard_key(void *context, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
    (void)keyboard; (void)serial; (void)time;
    av_wayland_t *client = context;
    if (key >= AvInputKeys || !av_input_scan_code(key) || state > WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (lease_mode(client)) {
        int suppressed = client->suppressed_keys[key];
        client->observed_keys[key] = (uint8_t)state;
        if (state == WL_KEYBOARD_KEY_STATE_RELEASED) client->suppressed_keys[key] = 0;
        if (!client->keyboard_window || client->lease.phase != AvLeaseForwarding ||
            !controls_enabled(client->keyboard_window)) {
            if (state) client->suppressed_keys[key] = 1;
            return;
        }
        if (suppressed) return;
    } else {
        if (!client->keyboard_window) return;
        if (client->suppressed_keys[key]) {
            if (state == WL_KEYBOARD_KEY_STATE_RELEASED) client->suppressed_keys[key] = 0;
            return;
        }
    }
    send_input(client, client->keyboard_window, MsgInputKey, state, key, 0, 0);
}
static void keyboard_modifiers(void *context, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    (void)context; (void)keyboard; (void)serial; (void)depressed;
    (void)latched; (void)locked; (void)group;
    // Modifier physical key events are forwarded; XKB state/text is not translated.
}
static void keyboard_repeat(void *context, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)context; (void)keyboard; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener KeyboardEvents = {.keymap = keyboard_keymap,
    .enter = keyboard_enter, .leave = keyboard_leave, .key = keyboard_key,
    .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat};
static void clear_pointer_focus(av_wayland_t *client) {
    if (client->pointer_window && client->pointer_window == client->keyboard_window)
        send_input(client, client->keyboard_window, MsgInputRelease, AvInputReleaseButtons, 0, 0, 0);
    suppress_observed(client, AvInputReleaseButtons);
    client->pointer_window = NULL;
}
static void pointer_enter(void *context, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer; (void)serial;
    av_wayland_t *client = context;
    video_window_t *window = input_surface(client, surface);
    if (lease_mode(client)) {
        if (!lease_ready(client)) window = NULL;
        if (client->pointer_window != window) clear_pointer_focus(client);
        if (!lease_ready(client)) window = NULL; /* Release enqueue may have failed. */
    }
    client->pointer_window = window;
    client->pointer_x = x; client->pointer_y = y;
    forward_pointer(client);
}
static void pointer_leave(void *context, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)pointer; (void)serial;
    av_wayland_t *client = context;
    if (client->pointer_window && client->pointer_window->surface == surface)
        clear_pointer_focus(client);
}
static void pointer_motion(void *context, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
    (void)pointer; (void)time;
    av_wayland_t *client = context;
    client->pointer_x = x; client->pointer_y = y;
    forward_pointer(client);
}
static void pointer_button(void *context, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    av_wayland_t *client = context;
    // Linux input-event-codes BTN_LEFT/RIGHT/MIDDLE, no extra buttons guessed.
    uint32_t code = button >= 0x110 && button <= 0x112 ? button - 0x110 + 1 : 0;
    if (!code || state > WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (lease_mode(client)) {
        int suppressed = client->suppressed_buttons[code];
        int was_down = client->observed_buttons[code];
        client->observed_buttons[code] = (uint8_t)state;
        if (!state) client->suppressed_buttons[code] = 0;
        if (!client->pointer_window || client->pointer_window != client->keyboard_window ||
            client->lease.phase != AvLeaseForwarding || !controls_enabled(client->keyboard_window) ||
            (state && !input_coordinates_valid(client))) {
            if (state) client->suppressed_buttons[code] = 1;
            return;
        }
        if (suppressed || (state && was_down)) return;
        if (state && forward_pointer(client) != 0) return;
    } else if (!client->pointer_window || client->pointer_window != client->keyboard_window ||
               (state && !input_coordinates_valid(client))) return;
    send_input(client, client->keyboard_window, MsgInputButton, state, code, 0, 0);
}
static void pointer_axis(void *context, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)pointer; (void)time;
    av_wayland_t *client = context;
    if (!input_coordinates_valid(client)) return;
    // Baseline continuous scrolling: ten surface units correspond to 120 wheel units.
    int64_t units = (int64_t)value * 12 / 256;
    if (!units || units < -1200 || units > 1200 ||
        (axis != WL_POINTER_AXIS_VERTICAL_SCROLL && axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL)) return;
    if (lease_mode(client) && forward_pointer(client) != 0) return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        send_input(client, client->keyboard_window, MsgInputWheel, 0, 0, 0, -(int32_t)units);
    else
        send_input(client, client->keyboard_window, MsgInputWheel, 0, 0, (int32_t)units, 0);
}
static void pointer_frame(void *context, struct wl_pointer *pointer) { (void)context; (void)pointer; }
static void pointer_source(void *context, struct wl_pointer *pointer, uint32_t source) {
    (void)context; (void)pointer; (void)source;
}
static void pointer_stop(void *context, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)context; (void)pointer; (void)time; (void)axis;
}
static void pointer_discrete(void *context, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)context; (void)pointer; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener PointerEvents = {.enter = pointer_enter, .leave = pointer_leave,
    .motion = pointer_motion, .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_source, .axis_stop = pointer_stop, .axis_discrete = pointer_discrete};
static void seat_capabilities(void *context, struct wl_seat *seat, uint32_t capabilities) {
    av_wayland_t *client = context;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !client->keyboard && !client->shutting_down) {
        client->keyboard = wl_seat_get_keyboard(seat);
        if (!client->keyboard || wl_keyboard_add_listener(client->keyboard, &KeyboardEvents, client) != 0)
            mark_delivery_failed(client);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && client->keyboard) {
        clear_keyboard_focus(client);
        wl_keyboard_release(client->keyboard); client->keyboard = NULL; client->keymap_ready = 0;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !client->pointer && !client->shutting_down) {
        client->pointer = wl_seat_get_pointer(seat);
        if (!client->pointer || wl_pointer_add_listener(client->pointer, &PointerEvents, client) != 0)
            mark_delivery_failed(client);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && client->pointer) {
        if (lease_mode(client)) clear_pointer_focus(client);
        else {
            send_input(client, client->keyboard_window, MsgInputRelease, AvInputReleaseButtons, 0, 0, 0);
            client->pointer_window = NULL;
        }
        wl_pointer_release(client->pointer); client->pointer = NULL;
    }
}
static void seat_name(void *context, struct wl_seat *seat, const char *name) {
    (void)context; (void)seat; (void)name;
}
static const struct wl_seat_listener SeatEvents = {.capabilities = seat_capabilities, .name = seat_name};

static void lease_done(void *context, struct wl_callback *callback, uint32_t callback_data) {
    (void)callback_data; /* Compositor serial is not the owned lease epoch. */
    av_wayland_t *client = context;
    if (callback != client->lease_sync) return; /* Never destroy an unowned callback. */
    if (!lease_mode(client) || client->shutting_down || client->delivery_failed ||
        client->lease.phase != AvLeaseBarrier || client->lease_sync_epoch != client->lease.epoch) {
        fail_input(client);
        return;
    }
    av_message_t ack;
    if (av_lease_host_ack(&client->lease, monotonic_ms(), &ack) != 0) {
        fail_input(client);
        return;
    }
    cancel_lease_sync(client);
    if (client->request(&ack, client->request_context) != 0) fail_input(client);
}
static const struct wl_callback_listener LeaseEvents = {.done = lease_done};
static int lease_message(av_wayland_t *client, const av_message_t *message) {
    if (!lease_mode(client)) return fail_input(client);
    int result = av_lease_host_receive(&client->lease, message, monotonic_ms());
    if (result < 0) return fail_input(client);
    if (result == 1) {
        disable_routes(client);
        if (client->lease_sync) return fail_input(client);
        client->lease_sync_epoch = client->lease.epoch;
        client->lease_sync = wl_display_sync(client->display);
        if (!client->lease_sync || wl_callback_add_listener(client->lease_sync, &LeaseEvents, client) != 0)
            return fail_input(client);
        /* The peer pump follows the ordinary display flush. Flush this new sync
         * immediately; EAGAIN requests POLLOUT with the original deadline. */
        if (av_wayland_flush(client) != 0) return fail_input(client);
    }
    return 0;
}

static void buffer_release(void *context, struct wl_buffer *buffer) {
    (void)buffer;
    video_buffer_t *video = context;
    if (video->busy) {
        av_video_release(video->slot);
        video->busy = 0;
    }
}
static const struct wl_buffer_listener BufferEvents = {.release = buffer_release};
static void commit_done(void *context, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    av_wayland_t *client = context;
    wl_callback_destroy(callback);
    client->watch_sync = NULL;
    video_window_t *window = find_window(client, client->watch_window);
    client->watch_window = 0;
    if (controls_enabled(window) && window->incarnation == client->watch_incarnation)
        client->watch_done(client->watch_context);
}
static const struct wl_callback_listener CommitEvents = {.done = commit_done};
int av_wayland_watch(av_wayland_t *client, uint64_t window_id, uint32_t token,
                     const uint8_t *mapping, size_t length, av_commit_done_t done, void *context) {
    video_window_t *window = find_window(client, window_id);
    if (!controls_enabled(window) || client->watch_window || !window_id || !token || token > 0xffffff || !mapping || !done)
        return -1;
    client->watch_window = window_id;
    client->watch_incarnation = window->incarnation;
    client->watch_token = token;
    client->watch_mapping = mapping;
    client->watch_length = length;
    client->watch_done = done;
    client->watch_context = context;
    return 0;
}
static void attach_buffer(video_buffer_t *video) {
    video_window_t *window = video->window;
    video->busy = 1;
    window->input_width = video->width;
    window->input_height = video->height;
    wl_surface_attach(window->surface, video->buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, (int32_t)video->width, (int32_t)video->height);
    xdg_surface_set_window_geometry(window->xdg_surface, 0, 0,
                                   (int32_t)video->width, (int32_t)video->height);
    wl_surface_commit(window->surface);
    av_wayland_t *client = window->client;
    if (client->watch_window == window->geometry.window_id && !client->watch_sync &&
        video->offset <= client->watch_length && window->capacity <= client->watch_length - video->offset &&
        av_target_matches(client->watch_mapping + video->offset, window->capacity,
                          video->width, video->height, video->stride, client->watch_token)) {
        client->watch_sync = wl_display_sync(client->display);
        if (!client->watch_sync || wl_callback_add_listener(client->watch_sync, &CommitEvents, client) != 0)
            mark_delivery_failed(client);
    }
}
static void imported_buffer(void *context, struct zwp_linux_buffer_params_v1 *params,
                            struct wl_buffer *buffer) {
    video_buffer_t *video = context;
    zwp_linux_buffer_params_v1_destroy(params);
    video->params = NULL;
    if (!window_active(video->window)) {
        wl_buffer_destroy(buffer);
        av_video_release(video->slot);
        video->busy = 0;
        return;
    }
    video->buffer = buffer;
    wl_buffer_add_listener(buffer, &BufferEvents, video);
    attach_buffer(video);
}
static void rejected_buffer(void *context, struct zwp_linux_buffer_params_v1 *params) {
    video_buffer_t *video = context;
    video_window_t *window = video->window;
    zwp_linux_buffer_params_v1_destroy(params);
    video->params = NULL;
    window->dmabuf_failed = 1;
    if (window_active(window)) {
        video->buffer = wl_shm_pool_create_buffer(window->pool, (int32_t)video->offset,
            (int32_t)video->width, (int32_t)video->height, (int32_t)video->stride,
            WL_SHM_FORMAT_ARGB8888);
        if (video->buffer) {
            wl_buffer_add_listener(video->buffer, &BufferEvents, video);
            attach_buffer(video);
            return;
        }
        mark_delivery_failed(window->client);
    }
    av_video_release(video->slot);
    video->busy = 0;
}
static const struct zwp_linux_buffer_params_v1_listener ParamsEvents = {
    .created = imported_buffer, .failed = rejected_buffer};
static void surface_configure(void *context, struct xdg_surface *surface, uint32_t serial) {
    video_window_t *window = context;
    if (!window_active(window)) return;
    xdg_surface_ack_configure(surface, serial);
    window->configured = 1;
}
static const struct xdg_surface_listener SurfaceEvents = {.configure = surface_configure};
static void toplevel_configure(void *context, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states) {
    (void)toplevel;
    video_window_t *window = context;
    if (!controls_enabled(window)) return;
    av_message_t request = window->geometry;
    request.type = MsgWindowGeometry;
    request.sequence = window->incarnation;
    request.flags &= ~AvWindowFullscreen;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            request.flags |= AvWindowFullscreen;
    }
    if (width >= 0 && height >= 0 && width <= 8192 && height <= 8192) {
        if (width) request.width = (uint32_t)width;
        if (height) request.height = (uint32_t)height;
    }
    /* Zero dimensions mean the client chooses its size; state transitions still
     * apply. Preserve guest dimensions while forwarding fullscreen changes. */
    if (request.width != window->geometry.width || request.height != window->geometry.height ||
        request.flags != window->geometry.flags) {
        if (window->client->request(&request, window->client->request_context) != 0)
            mark_delivery_failed(window->client);
    }
}
static void toplevel_close(void *context, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    video_window_t *window = context;
    if (!controls_enabled(window)) return;
    av_message_t request = {.type = MsgWindowClose, .window_id = window->geometry.window_id,
                            .sequence = window->incarnation};
    if (window->client->request(&request, window->client->request_context) != 0)
        mark_delivery_failed(window->client);
}
static const struct xdg_toplevel_listener ToplevelEvents = {.configure = toplevel_configure,
                                                            .close = toplevel_close};
static void shell_ping(void *context, struct xdg_wm_base *shell, uint32_t serial) {
    (void)context;
    xdg_wm_base_pong(shell, serial);
}
static const struct xdg_wm_base_listener ShellEvents = {.ping = shell_ping};
static void dmabuf_format(void *context, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format) {
    (void)context;
    (void)dmabuf;
    (void)format;
}
static void dmabuf_modifier(void *context, struct zwp_linux_dmabuf_v1 *dmabuf, uint32_t format,
                            uint32_t high, uint32_t low) {
    (void)dmabuf;
    av_wayland_t *client = context;
    if (format == AvPixelFormat && high == 0 && low == 0)
        client->linear_argb = 1;
    else if (format == AvPixelFormat && high == 0x00ffffff && low == UINT32_MAX &&
             client->linear_argb != 1)
        client->linear_argb = 2; /* Advertised implicit modifier; async import may fail. */
}
static const struct zwp_linux_dmabuf_v1_listener DmabufEvents = {.format = dmabuf_format,
                                                                 .modifier = dmabuf_modifier};
static void registry_global(void *context, struct wl_registry *registry, uint32_t name,
                            const char *interface_name, uint32_t version) {
    av_wayland_t *client = context;
    if (!strcmp(interface_name, "wl_seat") && version >= 5 && !client->seat && !client->shutting_down) {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        client->seat_global = name;
        if (!client->seat || wl_seat_add_listener(client->seat, &SeatEvents, client) != 0)
            mark_delivery_failed(client);
    } else if (!strcmp(interface_name, "wl_compositor") && version >= 4 && !client->compositor)
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface_name, "wl_shm") && !client->shm)
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface_name, "zwp_linux_dmabuf_v1") && version >= 3 && !client->dmabuf) {
        client->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        if (client->dmabuf)
            zwp_linux_dmabuf_v1_add_listener(client->dmabuf, &DmabufEvents, client);
    } else if (!strcmp(interface_name, "xdg_wm_base") && !client->shell) {
        client->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        if (client->shell)
            xdg_wm_base_add_listener(client->shell, &ShellEvents, client);
    }
}
static void registry_remove(void *context, struct wl_registry *registry, uint32_t name) {
    av_wayland_t *client = context;
    (void)registry;
    if (client->seat && name == client->seat_global) {
        clear_keyboard_focus(client);
        seat_capabilities(client, client->seat, 0);
        wl_seat_release(client->seat); client->seat = NULL; client->seat_global = 0;
    }
}
static const struct wl_registry_listener RegistryEvents = {.global = registry_global,
                                                           .global_remove = registry_remove};
static int busy(video_window_t *window) {
    return window->buffers[0].busy || window->buffers[1].busy || window->buffers[2].busy;
}
static void retire_window(video_window_t *window) {
    // Guest removal already releases this incarnation. Do not enqueue stale
    // focus-clear requests while retiring or draining disconnected surfaces.
    if (window->client) {
        if (window->client->watch_window == window->geometry.window_id &&
            window->client->watch_incarnation == window->incarnation) {
            if (window->client->watch_sync) wl_callback_destroy(window->client->watch_sync);
            window->client->watch_sync = NULL;
            window->client->watch_window = 0;
        }
        if (window->client->keyboard_window == window) {
            window->client->keyboard_window = NULL;
            if (lease_mode(window->client)) {
                suppress_observed(window->client, AvInputReleaseKeys | AvInputReleaseButtons);
                if (window->client->lease.phase == AvLeaseForwarding)
                    window->client->lease.phase = AvLeaseHostUnfocusedReady;
            } else memset(window->client->suppressed_keys, 0, sizeof(window->client->suppressed_keys));
        }
        if (window->client->pointer_window == window) {
            suppress_observed(window->client, AvInputReleaseButtons);
            window->client->pointer_window = NULL;
        }
    }
    if (window->toplevel) {
        xdg_toplevel_destroy(window->toplevel);
        window->toplevel = NULL;
    }
    if (window->xdg_surface) {
        xdg_surface_destroy(window->xdg_surface);
        window->xdg_surface = NULL;
    }
    if (window->surface) {
        wl_surface_destroy(window->surface);
        window->surface = NULL;
    }
    window->retired = 1;
}
static void free_window(video_window_t *window) {
    retire_window(window);
    for (unsigned i = 0; i < 3; ++i) {
        if (window->buffers[i].params)
            zwp_linux_buffer_params_v1_destroy(window->buffers[i].params);
        if (window->buffers[i].buffer)
            wl_buffer_destroy(window->buffers[i].buffer);
    }
    if (window->pool)
        wl_shm_pool_destroy(window->pool);
    memset(window, 0, sizeof(*window));
}
static video_window_t *find_window(av_wayland_t *client, uint64_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < AvMaxWindows; ++i)
        if (client->windows[i].geometry.window_id == id && !client->windows[i].retired)
            return &client->windows[i];
    return NULL;
}
static void drain_buffers(av_wayland_t *client);
void av_wayland_free(av_wayland_t **client_pointer) {
    av_wayland_t *client = *client_pointer;
    if (!client)
        return;
    client->shutting_down = 1;
    client->lease.phase = AvLeaseHostTerminal;
    client->lease.deadline = 0;
    disable_routes(client);
    cancel_lease_sync(client);
    if (client->watch_sync) {
        wl_callback_destroy(client->watch_sync);
        client->watch_sync = NULL;
    }
    client->watch_window = 0;
    seat_capabilities(client, client->seat, 0);
    if (client->seat) { wl_seat_release(client->seat); client->seat = NULL; }
    drain_buffers(client);
    for (unsigned i = 0; i < AvMaxWindows; ++i)
        free_window(&client->windows[i]);
    if (client->dmabuf)
        zwp_linux_dmabuf_v1_destroy(client->dmabuf);
    if (client->shell)
        xdg_wm_base_destroy(client->shell);
    if (client->shm)
        wl_shm_destroy(client->shm);
    if (client->compositor)
        wl_compositor_destroy(client->compositor);
    if (client->registry)
        wl_registry_destroy(client->registry);
    if (client->display)
        wl_display_disconnect(client->display);
    free(client);
    *client_pointer = NULL;
}
int av_wayland_init(av_wayland_t **client_pointer, av_host_request_t request, void *context) {
    *client_pointer = NULL;
    if (!request)
        return -1;
    av_wayland_t *client = calloc(1, sizeof(*client));
    if (!client)
        return -1;
    *client_pointer = client;
    client->request = request;
    client->request_context = context;
    client->display = wl_display_connect(NULL);
    if (!client->display)
        goto fail;
    client->registry = wl_display_get_registry(client->display);
    if (!client->registry ||
        wl_registry_add_listener(client->registry, &RegistryEvents, client) < 0 ||
        wl_display_roundtrip(client->display) < 0 || !client->compositor || !client->shm ||
        !client->shell)
        goto fail;
    if (wl_display_roundtrip(client->display) < 0 || client->delivery_failed)
        goto fail;
    return 0;
fail:
    av_wayland_free(client_pointer);
    return -1;
}
int av_wayland_create(av_wayland_t *client, const av_message_t *message, int fd,
                      size_t mapping_size, window_slot_header_t *slots[3],
                      const uint64_t offsets[3], size_t capacity) {
    uint8_t check[AvControlBytes];
    if (client->shutting_down || client->delivery_failed ||
        (message->type != MsgWindowCreate && message->type != MsgWindowCreateV2 &&
         message->type != MsgWindowCreateV3) ||
        av_control_encode(message, check, sizeof(check)) != 0 ||
        fd < 0 || mapping_size > INT32_MAX || !mapping_size || !capacity ||
        find_window(client, message->window_id))
        return -1;
    if (lease_mode(client) && av_lease_host_check(&client->lease, monotonic_ms()) != 0)
        return fail_input(client);
    for (unsigned i = 0; i < 3; ++i)
        if (!slots[i] || offsets[i] > mapping_size || capacity > mapping_size - offsets[i])
            return -1;
    /* Geometry.buffer_index may change; admission pool ownership never does. */
    for (unsigned i = 0; i < AvMaxWindows; ++i) {
        video_window_t *candidate = &client->windows[i];
        if (candidate->geometry.window_id && (!candidate->retired || busy(candidate)) &&
            candidate->pool_index == message->buffer_index) return -1;
    }
    if (av_identity_admit(&client->identity, message) != 0) return -1;
    video_window_t *window = NULL;
    for (unsigned i = 0; i < AvMaxWindows; ++i) {
        video_window_t *candidate = &client->windows[i];
        if (candidate->retired && !busy(candidate))
            free_window(candidate);
        if (!candidate->geometry.window_id) {
            window = candidate;
            break;
        }
    }
    if (!window)
        return -1;
    window->client = client;
    window->geometry = *message;
    window->incarnation = message->type != MsgWindowCreate ? message->sequence : 0;
    window->pool_index = message->buffer_index;
    if (!window->incarnation)
        fputs("AV host: legacy Create is display-only; upgrade both peers for input, resize and close\n", stderr);
    window->capacity = capacity;
    window->device_fd = fd;
    window->pool = wl_shm_create_pool(client->shm, fd, (int32_t)mapping_size);
    window->surface = wl_compositor_create_surface(client->compositor);
    if (!window->pool || !window->surface)
        goto fail;
    window->xdg_surface = xdg_wm_base_get_xdg_surface(client->shell, window->surface);
    if (!window->xdg_surface)
        goto fail;
    xdg_surface_add_listener(window->xdg_surface, &SurfaceEvents, window);
    window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
    if (!window->toplevel)
        goto fail;
    xdg_toplevel_add_listener(window->toplevel, &ToplevelEvents, window);
    xdg_toplevel_set_title(window->toplevel, message->title);
    xdg_toplevel_set_app_id(window->toplevel, "waddle.av");
    for (unsigned i = 0; i < 3; ++i) {
        window->buffers[i].slot = slots[i];
        window->buffers[i].offset = offsets[i];
        window->buffers[i].window = window;
    }
    wl_surface_commit(window->surface);
    return av_wayland_flush(client);
fail:
    free_window(window);
    return -1;
}
int av_wayland_message(av_wayland_t *client, const av_message_t *message) {
    uint8_t check[AvControlBytes];
    if (client->shutting_down || client->delivery_failed ||
        av_control_encode(message, check, sizeof(check)) != 0 ||
        client->identity.mode == AvIdentityUnknown) return fail_input(client);
    /* Lease serials are not frame indices and lease IDs are intentionally zero. */
    if (message->type == MsgInputEpochRevoked || message->type == MsgInputEpochReady)
        return lease_message(client, message);
    if (message->type != MsgWindowDestroy && message->type != MsgWindowGeometry &&
        message->type != MsgFrameReady) return fail_input(client);
    if (lease_mode(client) && av_lease_host_check(&client->lease, monotonic_ms()) != 0)
        return fail_input(client);
    video_window_t *window = find_window(client, message->window_id);
    if (client->identity.mode >= AvIdentityModern && message->type != MsgFrameReady) {
        int match = av_identity_match(window ? &window->geometry : NULL, message);
        if (match != 1) return match;
    }
    if (!window) return 0;
    if (message->type == MsgWindowDestroy) {
        retire_window(window);
    } else if (message->type == MsgWindowGeometry) {
        uint32_t previous_flags = window->geometry.flags;
        uint32_t process_id = window->geometry.process_id;
        window->geometry = *message;
        if (!window->geometry.process_id) window->geometry.process_id = process_id;
        if (message->flags & AvWindowMinimized)
            xdg_toplevel_set_minimized(window->toplevel);
        if ((previous_flags ^ message->flags) & AvWindowFullscreen) {
            if (message->flags & AvWindowFullscreen)
                xdg_toplevel_set_fullscreen(window->toplevel, NULL);
            else
                xdg_toplevel_unset_fullscreen(window->toplevel);
        }
        /* Size follows attached pixels; no global positioning API in xdg-shell. */
    } else if (message->type == MsgFrameReady) {
        video_buffer_t *video = &window->buffers[message->buffer_index];
        if (video->busy || !av_video_begin_read(video->slot))
            return 0;
        window_slot_header_t *slot = video->slot;
        /* Snapshot metadata once before validating untrusted shared storage. */
        const uint32_t width = slot->width, height = slot->height, stride = slot->stride;
        if (!window->configured || slot->frame_sequence != message->sequence ||
            width != message->width || height != message->height ||
            slot->format != AvPixelFormat || slot->buffer_index != message->buffer_index ||
            !av_video_size(width, height, stride, window->capacity) ||
            stride > INT32_MAX) {
            av_video_release(slot);
            return 0;
        }
        if (video->buffer && (video->width != width || video->height != height ||
                              video->stride != stride)) {
            wl_buffer_destroy(video->buffer);
            video->buffer = NULL;
        }
        if (!video->buffer) {
            video->width = width;
            video->height = height;
            video->stride = stride;
            if (client->dmabuf && client->linear_argb && !window->dmabuf_failed) {
                int exported = av_dmabuf_export(window->device_fd, video->offset, window->capacity);
                if (exported >= 0) {
                    struct zwp_linux_buffer_params_v1 *params =
                        zwp_linux_dmabuf_v1_create_params(client->dmabuf);
                    if (params) {
                        video->params = params;
                        video->busy = 1;
                        zwp_linux_buffer_params_v1_add_listener(params, &ParamsEvents, video);
                        zwp_linux_buffer_params_v1_add(params, exported, 0, 0, stride,
                            client->linear_argb == 1 ? 0 : 0x00ffffff,
                            client->linear_argb == 1 ? 0 : UINT32_MAX);
                        zwp_linux_buffer_params_v1_create(params, (int32_t)width,
                                                         (int32_t)height, AvPixelFormat, 0);
                    }
                    close(exported);
                    if (video->params) return av_wayland_flush(client);
                }
            }
            if (!video->buffer) {
                video->buffer = wl_shm_pool_create_buffer(
                    window->pool, (int32_t)video->offset, (int32_t)width,
                    (int32_t)height, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
            }
            if (!video->buffer) {
                av_video_release(slot);
                return -1;
            }
            video->width = width;
            video->height = height;
            video->stride = stride;
            wl_buffer_add_listener(video->buffer, &BufferEvents, video);
        }
        attach_buffer(video);
    } else
        return -1;
    return av_wayland_flush(client);
}
int av_wayland_dispatch(av_wayland_t *client) {
    if (av_wayland_timeout(client, 0) < 0 || wl_display_dispatch(client->display) < 0 ||
        av_wayland_timeout(client, 0) < 0) return fail_input(client);
    return 0;
}
int av_wayland_timeout(av_wayland_t *client, int maximum) {
    if (client->shutting_down || client->delivery_failed || maximum < 0) return -1;
    if (!lease_mode(client)) return maximum;
    uint64_t now = monotonic_ms();
    if (av_lease_host_check(&client->lease, now) != 0) return fail_input(client);
    return av_lease_timeout(client->lease.deadline, now, maximum);
}
int av_wayland_fd(av_wayland_t *client) { return wl_display_get_fd(client->display); }
int av_wayland_flush(av_wayland_t *client) {
    int result = wl_display_flush(client->display);
    client->writable = result < 0 && errno == EAGAIN;
    return result < 0 && !client->writable ? -1 : 0;
}
int av_wayland_writable(const av_wayland_t *client) { return client->writable; }
static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000 || (uint64_t)now.tv_sec > (UINT64_MAX - 999) / 1000) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static void drain_buffers(av_wayland_t *client) {
    if (!client->display) return;
    for (unsigned i = 0; i < AvMaxWindows; ++i) retire_window(&client->windows[i]);
    uint64_t now = monotonic_ms();
    if (!now || now > UINT64_MAX - 2000) return;
    uint64_t deadline = now + 2000;
    for (;;) {
        if (wl_display_dispatch_pending(client->display) < 0 || av_wayland_flush(client) != 0)
            break;
        int pending = 0;
        for (unsigned i = 0; i < AvMaxWindows; ++i) pending |= busy(&client->windows[i]);
        now = monotonic_ms();
        if (!pending || !now || now >= deadline) break;
        uint64_t remaining = deadline - now;
        struct pollfd display = {av_wayland_fd(client), POLLIN | (client->writable ? POLLOUT : 0), 0};
        int ready = poll(&display, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || display.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (display.revents & POLLIN && wl_display_dispatch(client->display) < 0) break;
    }
}
