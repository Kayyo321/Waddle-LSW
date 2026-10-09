#include "av_wayland.h"
#include "av_dmabuf.h"
#include "av_input.h"
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
    av_host_request_t request;
    void *request_context;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    video_window_t *keyboard_window, *pointer_window;
    uint32_t seat_global, input_serial;
    uint8_t suppressed_keys[AvInputKeys];
    wl_fixed_t pointer_x, pointer_y;
    int keymap_ready, shutting_down;
    int delivery_failed;
    int writable;
    uint64_t watch_window;
    uint32_t watch_token;
    const uint8_t *watch_mapping;
    size_t watch_length;
    av_commit_done_t watch_done;
    void *watch_context;
    struct wl_callback *watch_sync;
};
static video_window_t *input_surface(av_wayland_t *client, struct wl_surface *surface) {
    if (!surface || client->shutting_down) return NULL;
    for (unsigned i = 0; i < AvMaxWindows; ++i) {
        video_window_t *window = &client->windows[i];
        if (window->surface == surface && !window->retired && window->incarnation)
            return window;
    }
    return NULL;
}
static void send_input(av_wayland_t *client, video_window_t *window, uint32_t type,
                        uint32_t flags, uint32_t code, int32_t x, int32_t y) {
    if (!window || !window->incarnation || window->retired || client->delivery_failed ||
        client->shutting_down) return;
    if (client->input_serial == UINT32_MAX) { client->delivery_failed = 1; return; }
    av_message_t event = {.type = type, .window_id = window->geometry.window_id,
        .sequence = window->incarnation, .buffer_index = ++client->input_serial,
        .flags = flags, .width = code, .x = x, .y = y};
    if (client->request(&event, client->request_context) != 0) client->delivery_failed = 1;
}
static void clear_keyboard_focus(av_wayland_t *client) {
    send_input(client, client->keyboard_window, MsgInputFocus, 0, 0, 0, 0);
    client->keyboard_window = NULL;
    memset(client->suppressed_keys, 0, sizeof(client->suppressed_keys));
}
static int input_coordinates_valid(av_wayland_t *client) {
    video_window_t *window = client->pointer_window;
    if (!window || window != client->keyboard_window || client->pointer_x < 0 || client->pointer_y < 0)
        return 0;
    int32_t x = wl_fixed_to_int(client->pointer_x), y = wl_fixed_to_int(client->pointer_y);
    return (uint32_t)x < window->input_width && (uint32_t)y < window->input_height;
}
static void forward_pointer(av_wayland_t *client) {
    if (input_coordinates_valid(client))
        send_input(client, client->pointer_window, MsgInputPointer, 0, 0,
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
static void keyboard_enter(void *context, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard; (void)serial;
    av_wayland_t *client = context;
    clear_keyboard_focus(client);
    video_window_t *window = input_surface(client, surface);
    if (!window || !client->keymap_ready) return;
    if (!keys || keys->size % sizeof(uint32_t) || keys->size > AvInputKeys * sizeof(uint32_t) ||
        (keys->size && !keys->data)) { client->delivery_failed = 1; return; }
    for (size_t offset = 0; offset < keys->size; offset += sizeof(uint32_t)) {
        uint32_t key;
        memcpy(&key, (const uint8_t *)keys->data + offset, sizeof(key));
        if (key < AvInputKeys) client->suppressed_keys[key] = 1;
    }
    client->keyboard_window = window;
    send_input(client, window, MsgInputFocus, 1, 0, 0, 0);
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
    if (!client->keyboard_window || key >= AvInputKeys || !av_input_scan_code(key) ||
        state > WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (client->suppressed_keys[key]) {
        if (state == WL_KEYBOARD_KEY_STATE_RELEASED) client->suppressed_keys[key] = 0;
        return;
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
static void pointer_enter(void *context, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer; (void)serial;
    av_wayland_t *client = context;
    client->pointer_window = input_surface(client, surface);
    client->pointer_x = x; client->pointer_y = y;
    forward_pointer(client);
}
static void pointer_leave(void *context, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)pointer; (void)serial;
    av_wayland_t *client = context;
    if (client->pointer_window && client->pointer_window->surface == surface) {
        if (client->pointer_window == client->keyboard_window)
            send_input(client, client->keyboard_window, MsgInputRelease, AvInputReleaseButtons, 0, 0, 0);
        client->pointer_window = NULL;
    }
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
    if (!client->pointer_window || client->pointer_window != client->keyboard_window ||
        state > WL_POINTER_BUTTON_STATE_PRESSED ||
        (state == WL_POINTER_BUTTON_STATE_PRESSED && !input_coordinates_valid(client))) return;
    // Linux input-event-codes BTN_LEFT/RIGHT/MIDDLE, no extra buttons guessed.
    uint32_t code = button >= 0x110 && button <= 0x112 ? button - 0x110 + 1 : 0;
    if (code) send_input(client, client->keyboard_window, MsgInputButton, state, code, 0, 0);
}
static void pointer_axis(void *context, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)pointer; (void)time;
    av_wayland_t *client = context;
    if (!input_coordinates_valid(client)) return;
    // Baseline continuous scrolling: ten surface units correspond to 120 wheel units.
    int64_t units = (int64_t)value * 12 / 256;
    if (!units || units < -1200 || units > 1200) return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        send_input(client, client->keyboard_window, MsgInputWheel, 0, 0, 0, -(int32_t)units);
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
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
            client->delivery_failed = 1;
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && client->keyboard) {
        clear_keyboard_focus(client);
        wl_keyboard_release(client->keyboard); client->keyboard = NULL; client->keymap_ready = 0;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !client->pointer && !client->shutting_down) {
        client->pointer = wl_seat_get_pointer(seat);
        if (!client->pointer || wl_pointer_add_listener(client->pointer, &PointerEvents, client) != 0)
            client->delivery_failed = 1;
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && client->pointer) {
        send_input(client, client->keyboard_window, MsgInputRelease, AvInputReleaseButtons, 0, 0, 0);
        client->pointer_window = NULL;
        wl_pointer_release(client->pointer); client->pointer = NULL;
    }
}
static void seat_name(void *context, struct wl_seat *seat, const char *name) {
    (void)context; (void)seat; (void)name;
}
static const struct wl_seat_listener SeatEvents = {.capabilities = seat_capabilities, .name = seat_name};

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
    client->watch_window = 0;
    client->watch_done(client->watch_context);
}
static const struct wl_callback_listener CommitEvents = {.done = commit_done};
int av_wayland_watch(av_wayland_t *client, uint64_t window_id, uint32_t token,
                     const uint8_t *mapping, size_t length, av_commit_done_t done, void *context) {
    if (client->watch_window || !window_id || !token || token > 0xffffff || !mapping || !done)
        return -1;
    client->watch_window = window_id;
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
            client->delivery_failed = 1;
    }
}
static void imported_buffer(void *context, struct zwp_linux_buffer_params_v1 *params,
                            struct wl_buffer *buffer) {
    video_buffer_t *video = context;
    zwp_linux_buffer_params_v1_destroy(params);
    video->params = NULL;
    if (video->window->retired) {
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
    if (!window->retired) {
        video->buffer = wl_shm_pool_create_buffer(window->pool, (int32_t)video->offset,
            (int32_t)video->width, (int32_t)video->height, (int32_t)video->stride,
            WL_SHM_FORMAT_ARGB8888);
        if (video->buffer) {
            wl_buffer_add_listener(video->buffer, &BufferEvents, video);
            attach_buffer(video);
            return;
        }
        window->client->delivery_failed = 1;
    }
    av_video_release(video->slot);
    video->busy = 0;
}
static const struct zwp_linux_buffer_params_v1_listener ParamsEvents = {
    .created = imported_buffer, .failed = rejected_buffer};
static void surface_configure(void *context, struct xdg_surface *surface, uint32_t serial) {
    video_window_t *window = context;
    xdg_surface_ack_configure(surface, serial);
    window->configured = 1;
}
static const struct xdg_surface_listener SurfaceEvents = {.configure = surface_configure};
static void toplevel_configure(void *context, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states) {
    (void)toplevel;
    video_window_t *window = context;
    av_message_t request = window->geometry;
    request.type = MsgWindowGeometry;
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
            window->client->delivery_failed = 1;
    }
}
static void toplevel_close(void *context, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    video_window_t *window = context;
    av_message_t request = {.type = MsgWindowClose, .window_id = window->geometry.window_id};
    if (window->client->request(&request, window->client->request_context) != 0)
        window->client->delivery_failed = 1;
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
            client->delivery_failed = 1;
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
        if (window->client->keyboard_window == window) {
            window->client->keyboard_window = NULL;
            memset(window->client->suppressed_keys, 0, sizeof(window->client->suppressed_keys));
        }
        if (window->client->pointer_window == window) window->client->pointer_window = NULL;
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
    if (client->watch_sync) {
        wl_callback_destroy(client->watch_sync);
        client->watch_sync = NULL;
    }
    client->watch_window = 0;
    client->shutting_down = 1;
    client->keyboard_window = NULL;
    client->pointer_window = NULL;
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
    if (message->type != MsgWindowCreate || av_control_encode(message, check, sizeof(check)) != 0 ||
        fd < 0 || mapping_size > INT32_MAX || !mapping_size || !capacity ||
        find_window(client, message->window_id))
        return -1;
    for (unsigned i = 0; i < 3; ++i)
        if (!slots[i] || offsets[i] > mapping_size || capacity > mapping_size - offsets[i])
            return -1;
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
    window->incarnation = message->sequence;
    if (!window->incarnation)
        fputs("AV host: legacy zero-incarnation window is view-only; matching guest required for input\n", stderr);
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
    if (av_control_encode(message, check, sizeof(check)) != 0)
        return -1;
    video_window_t *window = find_window(client, message->window_id);
    if (!window)
        return -1;
    if (message->type == MsgWindowDestroy) {
        retire_window(window);
    } else if (message->type == MsgWindowGeometry) {
        uint32_t previous_flags = window->geometry.flags;
        window->geometry = *message;
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
    if (wl_display_dispatch(client->display) < 0 || client->delivery_failed)
        return -1;
    return 0;
}
int av_wayland_fd(av_wayland_t *client) { return wl_display_get_fd(client->display); }
int av_wayland_flush(av_wayland_t *client) {
    int result = wl_display_flush(client->display);
    client->writable = result < 0 && errno == EAGAIN;
    return result < 0 && !client->writable ? -1 : 0;
}
int av_wayland_writable(const av_wayland_t *client) { return client->writable; }
static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void drain_buffers(av_wayland_t *client) {
    if (!client->display) return;
    for (unsigned i = 0; i < AvMaxWindows; ++i) retire_window(&client->windows[i]);
    int64_t deadline = monotonic_ms() + 2000;
    for (;;) {
        if (wl_display_dispatch_pending(client->display) < 0 || av_wayland_flush(client) != 0)
            break;
        int pending = 0;
        for (unsigned i = 0; i < AvMaxWindows; ++i) pending |= busy(&client->windows[i]);
        int64_t remaining = deadline - monotonic_ms();
        if (!pending || remaining <= 0) break;
        struct pollfd display = {av_wayland_fd(client), POLLIN | (client->writable ? POLLOUT : 0), 0};
        int ready = poll(&display, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || display.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (display.revents & POLLIN && wl_display_dispatch(client->display) < 0) break;
    }
}
