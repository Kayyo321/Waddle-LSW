/** @file present_integration.c @brief Real Wayland wire/FD presentation to a mock compositor. */
#include "linux_dmabuf_client.h"
#include "linux_dmabuf_server.h"
#include "waddle/venus_frame.h"
#include "waddle/venus_present.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

/** @brief Mock server context, server thread owns counters/resources. */
typedef struct server_t {
    struct wl_display *display;
    struct wl_listener disconnected;
    struct stat allocation;
    _Atomic int reject;
    unsigned commits, damage, imports, rejects;
} server_t;
/** @brief Native mock surface, resources owned until protocol destruction. */
typedef struct surface_t {
    server_t *server;
    struct wl_resource *buffer;
    struct wl_resource *callback;
} surface_t;
/** @brief Native mock import, owns FD until parameters/buffer destruction. */
typedef struct import_t {
    server_t *server;
    int fd;
} import_t;
static void destroy_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}
static void import_free(struct wl_resource *resource) {
    import_t *import = wl_resource_get_user_data(resource);
    if (import->fd >= 0)
        close(import->fd);
    free(import);
}
static const struct wl_buffer_interface BufferRequests = {.destroy = destroy_request};
static void plane_request(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                          uint32_t plane, uint32_t offset, uint32_t stride, uint32_t high,
                          uint32_t low) {
    (void)client;
    import_t *import = wl_resource_get_user_data(resource);
    assert(import->fd == -1 && !plane && !offset && stride == 128 && !high && !low);
    struct stat metadata;
    assert(!fstat(fd, &metadata));
    assert(metadata.st_dev == import->server->allocation.st_dev &&
           metadata.st_ino == import->server->allocation.st_ino);
    import->fd = fd; // Real libwayland SCM_RIGHTS ownership, no pixel map/copy.
}
static void create_request(struct wl_client *client, struct wl_resource *resource, int32_t width,
                           int32_t height, uint32_t format, uint32_t flags) {
    import_t *params = wl_resource_get_user_data(resource);
    assert(params->fd >= 0 && width == 32 && height == 16 && format == 0x34325241 && !flags);
    if (atomic_load_explicit(&params->server->reject, memory_order_acquire)) {
        params->server->rejects++;
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    import_t *buffer = calloc(1, sizeof(*buffer));
    assert(buffer);
    buffer->server = params->server;
    buffer->fd = fcntl(params->fd, F_DUPFD_CLOEXEC, 0);
    assert(buffer->fd >= 0);
    struct wl_resource *proxy = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    assert(proxy);
    wl_resource_set_implementation(proxy, &BufferRequests, buffer, import_free);
    params->server->imports++;
    zwp_linux_buffer_params_v1_send_created(resource, proxy);
}
static const struct zwp_linux_buffer_params_v1_interface ParamsRequests = {
    .destroy = destroy_request, .add = plane_request, .create = create_request};
static void params_request(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    import_t *params = calloc(1, sizeof(*params));
    assert(params);
    params->server = wl_resource_get_user_data(resource);
    params->fd = -1;
    struct wl_resource *proxy =
        wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, 4, id);
    assert(proxy);
    wl_resource_set_implementation(proxy, &ParamsRequests, params, import_free);
}
static const struct zwp_linux_dmabuf_feedback_v1_interface FeedbackRequests = {.destroy =
                                                                                   destroy_request};
static void feedback_request(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                             struct wl_resource *surface) {
    (void)resource;
    (void)surface;
    struct wl_resource *feedback =
        wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    assert(feedback);
    wl_resource_set_implementation(feedback, &FeedbackRequests, NULL, NULL);
    int fd = memfd_create("compositor-format-table", MFD_CLOEXEC);
    assert(fd >= 0);
    const unsigned char Table[16] = {0x41, 0x52, 0x32, 0x34};
    assert(write(fd, Table, sizeof(Table)) == (ssize_t)sizeof(Table));
    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, fd, sizeof(Table));
    close(fd);
    dev_t device = 1;
    uint16_t index = 0;
    struct wl_array main_device = {.size = sizeof(device), .data = &device};
    struct wl_array indices = {.size = sizeof(index), .data = &index};
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &main_device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &main_device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
}
static const struct zwp_linux_dmabuf_v1_interface DmabufRequests = {.destroy = destroy_request,
                                                                    .create_params = params_request,
                                                                    .get_surface_feedback =
                                                                        feedback_request};
static void bind_dmabuf(struct wl_client *client, void *context, uint32_t version, uint32_t id) {
    assert(version == 4);
    struct wl_resource *resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, 4, id);
    assert(resource);
    wl_resource_set_implementation(resource, &DmabufRequests, context, NULL);
}
static void surface_free(struct wl_resource *resource) {
    surface_t *surface = wl_resource_get_user_data(resource);
    if (surface->callback)
        wl_resource_destroy(surface->callback);
    free(surface);
}
static void attach_request(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)client;
    assert(buffer && !x && !y);
    ((surface_t *)wl_resource_get_user_data(resource))->buffer = buffer;
}
static void frame_request(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    surface_t *surface = wl_resource_get_user_data(resource);
    assert(!surface->callback);
    surface->callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    assert(surface->callback);
}
static void damage_request(struct wl_client *client, struct wl_resource *resource, int32_t x,
                           int32_t y, int32_t width, int32_t height) {
    (void)client;
    assert(x == 4 && y == 2 && width == 8 && height == 4);
    surface_t *surface = wl_resource_get_user_data(resource);
    surface->server->damage++;
}
static void commit_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    surface_t *surface = wl_resource_get_user_data(resource);
    assert(surface->buffer && surface->callback);
    surface->server->commits++;
    // Actual protocol events; no pixel mapping, rendering or CPU copy in this mock.
    wl_callback_send_done(surface->callback, surface->server->commits);
    wl_resource_destroy(surface->callback);
    surface->callback = NULL;
    wl_buffer_send_release(surface->buffer);
    surface->buffer = NULL;
}
static const struct wl_surface_interface SurfaceRequests = {.destroy = destroy_request,
                                                            .attach = attach_request,
                                                            .frame = frame_request,
                                                            .damage_buffer = damage_request,
                                                            .commit = commit_request};
static void surface_request(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    surface_t *surface = calloc(1, sizeof(*surface));
    assert(surface);
    surface->server = wl_resource_get_user_data(resource);
    struct wl_resource *proxy = wl_resource_create(client, &wl_surface_interface, 4, id);
    assert(proxy);
    wl_resource_set_implementation(proxy, &SurfaceRequests, surface, surface_free);
}
static const struct wl_compositor_interface CompositorRequests = {.create_surface =
                                                                      surface_request};
static void bind_compositor(struct wl_client *client, void *context, uint32_t version,
                            uint32_t id) {
    assert(version == 4);
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, 4, id);
    assert(resource);
    wl_resource_set_implementation(resource, &CompositorRequests, context, NULL);
}
static void disconnected_client(struct wl_listener *listener, void *data) {
    (void)data;
    server_t *server = wl_container_of(listener, server, disconnected);
    wl_display_terminate(server->display);
}
static void *run_server(void *context) {
    server_t *server = context;
    wl_display_run(server->display);
    return NULL;
}
/** @brief Client-only borrowed globals and completion state, no cross-thread mutation. */
typedef struct client_t {
    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    uint64_t completed_frame;
    venus_ring_status_t status;
} client_t;
static void global_event(void *context, struct wl_registry *registry, uint32_t id,
                         const char *interface, uint32_t version) {
    client_t *client = context;
    assert(version >= 4);
    if (!strcmp(interface, "wl_compositor"))
        client->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "zwp_linux_dmabuf_v1"))
        client->dmabuf = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 4);
}
static void removed_event(void *context, struct wl_registry *registry, uint32_t id) {
    (void)context;
    (void)registry;
    (void)id;
}
static const struct wl_registry_listener RegistryEvents = {.global = global_event,
                                                           .global_remove = removed_event};
static void completed(void *context, uint64_t frame, venus_ring_status_t status) {
    client_t *client = context;
    client->completed_frame = frame;
    client->status = status;
}
int main(void) {
    alarm(30);
    int source = memfd_create("native-import-fixture", MFD_CLOEXEC);
    assert(source >= 0 && !ftruncate(source, 4096));
    server_t server = {0};
    atomic_init(&server.reject, 0);
    assert(!fstat(source, &server.allocation));
    server.display = wl_display_create();
    assert(server.display);
    assert(wl_global_create(server.display, &wl_compositor_interface, 4, &server, bind_compositor));
    assert(
        wl_global_create(server.display, &zwp_linux_dmabuf_v1_interface, 4, &server, bind_dmabuf));
    int streams[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, streams));
    struct wl_client *server_client = wl_client_create(server.display, streams[0]);
    assert(server_client);
    server.disconnected.notify = disconnected_client;
    wl_client_add_destroy_listener(server_client, &server.disconnected);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_server, &server));
    struct wl_display *display = wl_display_connect_to_fd(streams[1]);
    assert(display);
    client_t client = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    assert(registry && !wl_registry_add_listener(registry, &RegistryEvents, &client));
    assert(wl_display_roundtrip(display) >= 0 && client.compositor && client.dmabuf);
    struct wl_surface *surface = wl_compositor_create_surface(client.compositor);
    assert(surface);
    venus_present_t *presenter = NULL;
    assert(venus_present_create(&presenter, display, client.dmabuf, surface, completed, &client) ==
           RingOk);
    assert(wl_display_roundtrip(display) >= 0); // Real table FD and array events.
    int bridge[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, bridge));
    assert(venus_frame_prepare(bridge[0]) == RingOk && venus_frame_prepare(bridge[1]) == RingOk);
    venus_frame_t frame = {.context = 1,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.x = 4, .y = 2, .width = 8, .height = 4}}};
    for (uint64_t sequence = 1; sequence <= 16; sequence++) {
        atomic_store_explicit(&server.reject, sequence == 8, memory_order_release);
        frame.frame = sequence;
        assert(venus_frame_send(bridge[0], &frame, &source, 1) == RingOk);
        venus_frame_t received;
        int fds[4];
        assert(venus_frame_receive(bridge[1], getpid(), 1, &received, fds) == RingOk);
        assert(venus_present_submit(presenter, received.frame, &received.layout, fds,
                                    received.layout.plane_count, received.damage,
                                    received.damage_count) == RingOk);
        venus_frame_fds_free(fds); // libwayland retains its actual kernel duplicates.
        unsigned attempts = 0;
        while (client.completed_frame != sequence) {
            assert(wl_display_roundtrip(display) >= 0);
            assert(++attempts <= 8);
        }
        assert(client.status == (sequence == 8 ? RingInvalid : RingOk));
        assert(wl_display_roundtrip(display) >= 0); // Drain independent frame callback/destroys.
    }
    assert(venus_present_free(&presenter) == RingOk && !presenter);
    wl_surface_destroy(surface);
    zwp_linux_dmabuf_v1_destroy(client.dmabuf);
    wl_compositor_destroy(client.compositor);
    wl_registry_destroy(registry);
    assert(wl_display_roundtrip(display) >= 0);
    wl_display_disconnect(display);
    assert(!pthread_join(thread, NULL));
    assert(server.imports == 15 && server.rejects == 1 && server.commits == 15 &&
           server.damage == 15);
    wl_display_destroy(server.display);
    close(source);
    close(bridge[0]);
    close(bridge[1]);
    return 0;
}
