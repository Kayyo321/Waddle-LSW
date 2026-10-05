#include "av_wayland.h"
#include "av_dmabuf.h"
#include "linux_dmabuf_client.h"
#include "xdg_shell_client.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
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
    int delivery_failed;
    int writable;
};
static void buffer_release(void *context, struct wl_buffer *buffer) {
    (void)buffer;
    video_buffer_t *video = context;
    if (video->busy) {
        av_video_release(video->slot);
        video->busy = 0;
    }
}
static const struct wl_buffer_listener BufferEvents = {.release = buffer_release};
static void attach_buffer(video_buffer_t *video) {
    video_window_t *window = video->window;
    video->busy = 1;
    wl_surface_attach(window->surface, video->buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, (int32_t)video->width, (int32_t)video->height);
    xdg_surface_set_window_geometry(window->xdg_surface, 0, 0,
                                   (int32_t)video->width, (int32_t)video->height);
    wl_surface_commit(window->surface);
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
    if (!strcmp(interface_name, "wl_compositor") && version >= 4 && !client->compositor)
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
    (void)context;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener RegistryEvents = {.global = registry_global,
                                                           .global_remove = registry_remove};
static int busy(video_window_t *window) {
    return window->buffers[0].busy || window->buffers[1].busy || window->buffers[2].busy;
}
static void retire_window(video_window_t *window) {
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
    if (wl_display_roundtrip(client->display) < 0)
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
