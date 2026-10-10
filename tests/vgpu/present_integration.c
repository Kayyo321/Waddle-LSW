/** @file present_integration.c @brief Real Wayland wire/FD presentation to a mock compositor. */
#include "linux_dmabuf_client.h"
#include "linux_dmabuf_server.h"
#include "waddle/venus_frame.h"
#include "waddle/venus_surface.h"
#ifdef VgpuRemoteImage
#include "waddle/venus_guest.h"
#include "waddle/venus_worker.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <sys/wait.h>
#endif
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
    int identity_known; /**< Local source identity or first remote import snapshot. */
    uint32_t offset, stride;
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
    assert(import->fd == -1 && !plane && offset == import->server->offset &&
           stride == import->server->stride && !high && !low);
    struct stat metadata;
    assert(!fstat(fd, &metadata));
    if (!import->server->identity_known) {
        import->server->allocation = metadata;
        import->server->identity_known = 1;
    }
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
static void completed(void *context, const venus_frame_t *frame, venus_ring_status_t status) {
    client_t *client = context;
    assert(frame->context == 1 && frame->resource_ids[0] == 2);
    client->completed_frame = frame->frame;
    client->status = status;
}
#ifdef VgpuRemoteImage
/** @brief Call-scoped guest/controller fixture state; owned by run_remote. */
typedef struct remote_fixture_t {
    venus_guest_t guest;   /**< Caller-owned negotiated frontend. */
    venus_worker_t worker; /**< Owned unreaped receiver process. */
    int frame_fd;          /**< Borrowed controller native endpoint through callback. */
} remote_fixture_t;
static venus_ring_status_t guest_call(void *context, const venus_request_t *request,
                                      const void *input, size_t length, venus_request_t *response,
                                      void *output, size_t capacity) {
    remote_fixture_t *remote = context;
    return venus_guest_exchange(&remote->guest, request, input, length, response, output, capacity);
}
static venus_ring_status_t guest_frame(remote_fixture_t *remote, const venus_frame_t *frame) {
    unsigned char bytes[VenusFrameBytes];
    assert(venus_frame_encode(frame, bytes, sizeof(bytes)) == RingOk);
    const venus_request_t Request = {.kind = RequestPresent,
                                     .payload_bytes = sizeof(bytes),
                                     .argument_zero = 1,
                                     .argument_one = 1};
    venus_request_t response;
    return guest_call(remote, &Request, bytes, sizeof(bytes), &response, NULL, 0);
}
static void guest_busy(remote_fixture_t *remote) {
    const venus_request_t Request = {.kind = RequestFree, .resource_id = 2};
    venus_request_t response;
    assert(guest_call(remote, &Request, NULL, 0, &response, NULL, 0) == RingAgain);
}
static void guest_release(remote_fixture_t *remote, uint64_t frame, venus_release_t *release) {
    const venus_request_t Request = {.kind = RequestPresentPoll, .argument_zero = frame};
    unsigned char bytes[VenusReleaseBytes];
    venus_request_t response;
    venus_ring_status_t status;
    unsigned attempts = 0;
    do {
        status = guest_call(remote, &Request, NULL, 0, &response, bytes, sizeof(bytes));
        assert(++attempts <= 1000);
        if (status == RingAgain)
            usleep(1000);
    } while (status == RingAgain);
    assert(status == RingOk && response.payload_bytes == sizeof(bytes));
    assert(venus_release_decode(release, bytes, sizeof(bytes)) == RingOk);
    assert(guest_call(remote, &Request, NULL, 0, &response, bytes, sizeof(bytes)) == RingInvalid);
}
#endif
static int present_image(int source, uint64_t offset, uint64_t stride, uint64_t size,
                         uint64_t extent, void *context) {
#ifdef VgpuRemoteImage
    remote_fixture_t *remote = context;
#else
    (void)context;
    assert(source >= 0);
#endif
    assert(offset <= UINT32_MAX && stride <= UINT32_MAX);
    server_t server = {.offset = (uint32_t)offset, .stride = (uint32_t)stride};
    atomic_init(&server.reject, 0);
    if (source >= 0) {
        assert(!fstat(source, &server.allocation));
        server.identity_known = 1;
    }
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
    int bridge[2] = {-1, -1};
    int32_t worker_pid = getpid();
#ifdef VgpuRemoteImage
    if (remote) {
        bridge[1] = remote->frame_fd;
        worker_pid = remote->worker.process_id;
    } else
#endif
    {
        assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, bridge));
        assert(venus_frame_prepare(bridge[0]) == RingOk);
    }
    venus_surface_t *presenter = NULL;
    assert(venus_surface_create_acknowledged(&presenter, bridge[1], worker_pid, 1, display,
                                             client.dmabuf, surface, completed, &client) == RingOk);
    assert(wl_display_roundtrip(display) >= 0); // Real table FD and array events.
    venus_frame_t frame = {.context = 1,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.offset = (uint32_t)offset,
                                                  .stride = (uint32_t)stride,
                                                  .size = size,
                                                  .extent = extent}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.x = 4, .y = 2, .width = 8, .height = 4}}};
    for (uint64_t sequence = 1; sequence <= 16; sequence++) {
        atomic_store_explicit(&server.reject, sequence == 8, memory_order_release);
        frame.frame = sequence;
#ifdef VgpuRemoteImage
        if (remote) {
            if (sequence == 1) {
                venus_frame_t invalid = frame;
                invalid.context = 2;
                assert(guest_frame(remote, &invalid) == RingInvalid);
                invalid.context = 1;
                invalid.layout.planes[0].extent *= 2;
                assert(guest_frame(remote, &invalid) == RingInvalid);
            }
            assert(guest_frame(remote, &frame) == RingOk);
            guest_busy(remote);
        } else
#endif
            assert(venus_frame_send(bridge[0], &frame, &source, 1) == RingOk);
        assert(venus_surface_poll(presenter) == RingOk);
        unsigned attempts = 0;
        while (client.completed_frame != sequence) {
            assert(wl_display_roundtrip(display) >= 0);
            assert(++attempts <= 8);
        }
        assert(client.status == (sequence == 8 ? RingInvalid : RingOk));
        venus_release_t release;
#ifdef VgpuRemoteImage
        if (remote)
            guest_busy(remote); // Compositor release alone is not guest consumption.
        else
#endif
            assert(venus_release_receive(bridge[0], getpid(), 1, &release) == RingAgain);
        assert(venus_surface_poll(presenter) == RingAgain); // Flush ack before next native receipt.
#ifdef VgpuRemoteImage
        if (remote) {
            guest_busy(remote); // Authenticated ack retains lease until operation13 consumption.
            guest_release(remote, sequence, &release);
        } else
#endif
            assert(venus_release_receive(bridge[0], getpid(), 1, &release) == RingOk);
        assert(release.context == 1 && release.frame == sequence &&
               release.status == client.status);
        assert(wl_display_roundtrip(display) >= 0); // Drain independent frame callback/destroys.
    }
    assert(venus_surface_free(&presenter) == RingOk && !presenter);
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
    if (source >= 0) {
        close(bridge[0]);
        close(bridge[1]);
    }
    return 0;
}

#ifdef VgpuHardwareImage
/** @brief Run real Venus hardware clear/export with call-scoped borrowed callback.
 * @param[in] present Nonnull callback borrows FD/layout until return after release.
 * @param[in,out] context Nullable call-scoped callback state.
 * @return Zero success, one failure. Owns receiver/exported FD, symmetric teardown.
 * @note Sole session thread, no concurrent renderer; hardware GPU required.
 */
extern int venus_gpu_image_fixture_run(int (*present)(int, uint64_t, uint64_t, uint64_t, uint64_t,
                                                      void *),
                                       void *context);
#endif
#ifdef VgpuRemoteImage
/** @brief Run bounded remote image packets through borrowed negotiated exchange.
 * @param[in] exchange Nonnull call-scoped guest callback borrowing bounded buffers.
 * @param[in,out] guest Nonnull borrowed callback state retained for run.
 * @param[in] present Nonnull callback borrows queried layout, no native FD.
 * @param[in,out] context Nullable borrowed presentation callback state.
 * @return Zero success/one failure; caller destroys old worker on failure.
 * @note Sole guest/controller thread, no retained storage or host renderer ownership.
 */
extern int venus_gpu_remote_image_fixture_run(
    venus_ring_status_t (*exchange)(void *, const venus_request_t *, const void *, size_t,
                                    venus_request_t *, void *, size_t),
    void *guest, int (*present)(uint64_t, uint64_t, uint64_t, uint64_t, void *), void *context);
static int present_remote(uint64_t offset, uint64_t stride, uint64_t size, uint64_t extent,
                          void *context) {
    return present_image(-1, offset, stride, size, extent, context);
}
static unsigned remote_descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory))
        count++;
    assert(!closedir(directory));
    return count;
}
static int run_remote(void) {
    int result = 1, mapping_fd = -1, streams[2] = {-1, -1}, frames[2] = {-1, -1};
    void *mapping = MAP_FAILED;
    remote_fixture_t remote = {.frame_fd = -1};
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    unsigned char scratch[4096];
    char executable[PATH_MAX];
    const char *configured = getenv("WADDLE_PRODUCTION_WORKER");
    if (!realpath(configured ? configured : "build/waddle_vgpu_worker", executable))
        goto cleanup;
    mapping_fd = memfd_create("mapped-image-fixture", MFD_CLOEXEC);
    if (mapping_fd < 0 || ftruncate(mapping_fd, 4096))
        goto cleanup;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, 0);
    if (mapping == MAP_FAILED || venus_region_init(mapping, 4096, 64) != RingOk ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, streams) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, frames) ||
        venus_frame_prepare(frames[0]) != RingOk || venus_frame_prepare(frames[1]) != RingOk)
        goto cleanup;
    if (venus_worker_create_presented(&remote.worker, executable, mapping_fd, streams[1], frames[1],
                                      1) != RingOk)
        goto cleanup;
    close(streams[1]);
    streams[1] = -1;
    close(frames[1]);
    frames[1] = -1;
    remote.frame_fd = frames[0];
    if (venus_session_init(&session, SessionGuest, mapping, 4096, 0) != RingOk ||
        venus_channel_init(&channel, &session, streams[0], NULL) != RingOk ||
        venus_channel_deadline(&channel, 5000) != RingOk ||
        venus_channel_handshake(&channel) != RingOk ||
        venus_rpc_init(&rpc, &channel, scratch, sizeof(scratch)) != RingOk ||
        venus_guest_init(&remote.guest, &rpc, 5000) != RingOk)
        goto cleanup;
    result = venus_gpu_remote_image_fixture_run(guest_call, &remote, present_remote, &remote);
    venus_guest_free(&remote.guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    close(streams[0]);
    streams[0] = -1;
    if (!result) {
        for (unsigned attempt = 0;; attempt++) {
            venus_ring_status_t status = venus_worker_poll(&remote.worker);
            if (status == RingClosed) {
                if (!WIFEXITED(remote.worker.exit_status) || WEXITSTATUS(remote.worker.exit_status))
                    result = 1;
                break;
            }
            if (status != RingAgain || attempt >= 5000) {
                result = 1;
                break;
            }
            usleep(1000);
        }
    }
cleanup:
    venus_guest_free(&remote.guest);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    if (venus_worker_destroy(&remote.worker, 1000) != RingOk)
        result = 1;
    for (unsigned index = 0; index < 2; index++) {
        if (streams[index] >= 0)
            close(streams[index]);
        if (frames[index] >= 0)
            close(frames[index]);
    }
    venus_region_detach(&session.region);
    if (mapping != MAP_FAILED)
        munmap(mapping, 4096);
    if (mapping_fd >= 0)
        close(mapping_fd);
    return result;
}
#endif
int main(int argc, char **argv) {
    alarm(90);
#ifdef VgpuRemoteImage
    if (argc == 2 && !strcmp(argv[1], "--require-hardware")) {
        unsigned baseline = remote_descriptors();
        for (unsigned iteration = 0; iteration < 3; iteration++) {
            int status = run_remote();
            if (status)
                return status;
            assert(remote_descriptors() == baseline);
        }
        puts("Mapped guest/worker hardware image, Wayland release and fresh-context churn passed");
        return 0;
    }
#endif
#ifdef VgpuHardwareImage
    if (argc == 2 && !strcmp(argv[1], "--require-hardware"))
        return venus_gpu_image_fixture_run(present_image, NULL);
#endif
    (void)argv;
    if (argc != 1)
        return 2;
    int source = memfd_create("native-import-fixture", MFD_CLOEXEC);
    assert(source >= 0 && !ftruncate(source, 4096));
    int result = present_image(source, 0, 128, 2048, 4096, NULL);
    close(source);
    return result;
}
