/** @file present.c @brief Native protocol-boundary pacing/import ownership faults. */
#include "linux_dmabuf_client.h"
#include "waddle/venus_present.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
static int fault, proxies, owners, disconnected, completions, commits, damage_calls, plane_calls;
static unsigned dmabuf_version = 4, surface_version = 4;
static uint64_t completed_frame;
static venus_ring_status_t completed_status;
static int display_cookie, dmabuf_cookie, surface_cookie;
static struct wl_display *display = (void *)&display_cookie;
static struct zwp_linux_dmabuf_v1 *dmabuf = (void *)&dmabuf_cookie;
static struct wl_surface *surface = (void *)&surface_cookie;
static void *new_proxy(void) {
    void *proxy = malloc(16);
    assert(proxy);
    proxies++;
    return proxy;
}
static void destroy_proxy(void *proxy) {
    assert(proxy && proxies > 0);
    proxies--;
    free(proxy);
}
static void *fixture_calloc(size_t count, size_t bytes) {
    if (fault == 1)
        return NULL;
    void *owner = calloc(count, bytes);
    assert(owner);
    owners++;
    return owner;
}
static void fixture_free(void *owner) {
    assert(owner && owners > 0);
    owners--;
    free(owner);
}
static uint32_t fixture_version(struct wl_proxy *proxy) {
    assert((void *)proxy == (void *)dmabuf || (void *)proxy == (void *)surface);
    return (void *)proxy == (void *)dmabuf ? dmabuf_version : surface_version;
}
static struct zwp_linux_dmabuf_feedback_v1 *fixture_feedback(struct zwp_linux_dmabuf_v1 *global,
                                                             struct wl_surface *target) {
    assert(global == dmabuf && target == surface);
    return fault == 2 ? NULL : new_proxy();
}
static int fixture_feedback_listener(struct zwp_linux_dmabuf_feedback_v1 *proxy,
                                     const struct zwp_linux_dmabuf_feedback_v1_listener *listener,
                                     void *context) {
    assert(proxy && listener && context);
    return fault == 3 ? -1 : 0;
}
static struct zwp_linux_buffer_params_v1 *fixture_params(struct zwp_linux_dmabuf_v1 *global) {
    assert(global == dmabuf);
    return fault == 4 ? NULL : new_proxy();
}
static int fixture_params_listener(struct zwp_linux_buffer_params_v1 *proxy,
                                   const struct zwp_linux_buffer_params_v1_listener *listener,
                                   void *context) {
    assert(proxy && listener && context);
    return fault == 5 ? -1 : 0;
}
static void fixture_plane(struct zwp_linux_buffer_params_v1 *proxy, int fd, uint32_t plane,
                          uint32_t offset, uint32_t stride, uint32_t high, uint32_t low) {
    assert(proxy && fcntl(fd, F_GETFD) >= 0 && plane < 2 && stride);
    assert(!high && !low && offset < 8192);
    plane_calls++;
}
static void fixture_create_buffer(struct zwp_linux_buffer_params_v1 *proxy, int32_t width,
                                  int32_t height, uint32_t format, uint32_t flags) {
    assert(proxy && width > 0 && height > 0 && format && !flags);
}
static int fixture_buffer_listener(struct wl_buffer *proxy,
                                   const struct wl_buffer_listener *listener, void *context) {
    assert(proxy && listener && context);
    return fault == 6 ? -1 : 0;
}
static struct wl_callback *fixture_frame(struct wl_surface *target) {
    assert(target == surface);
    return fault == 7 ? NULL : new_proxy();
}
static int fixture_frame_listener(struct wl_callback *proxy,
                                  const struct wl_callback_listener *listener, void *context) {
    assert(proxy && listener && context);
    return fault == 8 ? -1 : 0;
}
static void fixture_attach(struct wl_surface *target, struct wl_buffer *buffer, int32_t x,
                           int32_t y) {
    assert(target == surface && buffer && !x && !y);
}
static void fixture_damage(struct wl_surface *target, int32_t x, int32_t y, int32_t width,
                           int32_t height) {
    assert(target == surface && x >= 0 && y >= 0 && width > 0 && height > 0);
    damage_calls++;
}
static void fixture_commit(struct wl_surface *target) {
    assert(target == surface);
    commits++;
}
static int fixture_error(struct wl_display *connection) {
    assert(connection == display);
    return disconnected;
}
static int fixture_fstat(int fd, struct stat *metadata) {
    return fault == 9 ? -1 : fstat(fd, metadata);
}
static void *fixture_mmap(void *address, size_t bytes, int protection, int flags, int fd,
                          off_t offset) {
    assert(!address && protection == PROT_READ && flags == MAP_PRIVATE && !offset);
    return fault == 10 ? MAP_FAILED : mmap(address, bytes, protection, flags, fd, offset);
}
#define calloc fixture_calloc
#define free fixture_free
#define wl_proxy_get_version fixture_version
#define zwp_linux_dmabuf_v1_get_surface_feedback fixture_feedback
#define zwp_linux_dmabuf_feedback_v1_add_listener fixture_feedback_listener
#define zwp_linux_dmabuf_feedback_v1_destroy destroy_proxy
#define zwp_linux_dmabuf_v1_create_params fixture_params
#define zwp_linux_buffer_params_v1_add_listener fixture_params_listener
#define zwp_linux_buffer_params_v1_add fixture_plane
#define zwp_linux_buffer_params_v1_create fixture_create_buffer
#define zwp_linux_buffer_params_v1_destroy destroy_proxy
#define wl_buffer_add_listener fixture_buffer_listener
#define wl_buffer_destroy destroy_proxy
#define wl_surface_frame fixture_frame
#define wl_callback_add_listener fixture_frame_listener
#define wl_callback_destroy destroy_proxy
#define wl_surface_attach fixture_attach
#define wl_surface_damage_buffer fixture_damage
#define wl_surface_commit fixture_commit
#define wl_display_get_error fixture_error
#define fstat fixture_fstat
#define mmap fixture_mmap
#include "venus_present.c"

static void done(void *context, uint64_t frame, venus_ring_status_t status) {
    assert(context == &display_cookie && frame);
    completions++;
    completed_frame = frame;
    completed_status = status;
}
static venus_present_t *create(void) {
    venus_present_t *presenter = NULL;
    assert(venus_present_create(&presenter, display, dmabuf, surface, done, &display_cookie) ==
           RingOk);
    return presenter;
}
static void end(venus_present_t **presenter) {
    assert(venus_present_free(presenter) == RingOk && !*presenter);
    assert(!proxies && !owners);
}
static int table_fd(void) {
    int fd = memfd_create("feedback", MFD_CLOEXEC);
    assert(fd >= 0);
    const unsigned char Table[16] = {0x41, 0x52, 0x32, 0x34};
    assert(write(fd, Table, sizeof(Table)) == (ssize_t)sizeof(Table));
    return fd;
}
static void formats(venus_present_t *presenter) {
    dev_t device = 1;
    struct wl_array device_array = {.data = &device, .size = sizeof(device)};
    unsigned char indices[2] = {0};
    struct wl_array index_array = {.data = indices, .size = sizeof(indices)};
    feedback_main(presenter, presenter->feedback, &device_array);
    feedback_target(presenter, presenter->feedback, &device_array);
    feedback_flags(presenter, presenter->feedback, 0);
    feedback_formats(presenter, presenter->feedback, &index_array);
    feedback_tranche_done(presenter, presenter->feedback);
    feedback_done(presenter, presenter->feedback);
    assert(presenter->ready && !presenter->failed);
}
static void ready(venus_present_t *presenter) {
    int fd = table_fd();
    feedback_table(presenter, presenter->feedback, fd, 16);
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    formats(presenter);
}
static venus_dmabuf_layout_t layout = {.width = 64,
                                       .height = 32,
                                       .fourcc = 0x34325241,
                                       .plane_count = 1,
                                       .planes = {{.stride = 256, .size = 8192, .extent = 8192}}};
static venus_dmabuf_damage_t damage = {.width = 32, .height = 16};
static venus_ring_status_t submit(venus_present_t *presenter, uint64_t frame, int fd) {
    return venus_present_submit(presenter, frame, &layout, &fd, 1, &damage, 1);
}
static void import_slot(venus_present_t *presenter, unsigned index) {
    present_slot_t *slot = &presenter->slots[index];
    assert(slot->params);
    imported_buffer(slot, slot->params, new_proxy());
}
static void pace(venus_present_t *presenter) {
    assert(presenter->frame_callback);
    paced_frame(presenter, presenter->frame_callback, 1);
}
static void test_create(void) {
    venus_present_t *presenter = NULL;
    assert(venus_present_create(NULL, display, dmabuf, surface, done, NULL) == RingInvalid);
    presenter = (void *)1;
    assert(venus_present_create(&presenter, display, dmabuf, surface, done, NULL) == RingInvalid);
    presenter = NULL;
    assert(venus_present_create(&presenter, NULL, dmabuf, surface, done, NULL) == RingInvalid);
    assert(venus_present_create(&presenter, display, NULL, surface, done, NULL) == RingInvalid);
    assert(venus_present_create(&presenter, display, dmabuf, NULL, done, NULL) == RingInvalid);
    assert(venus_present_create(&presenter, display, dmabuf, surface, NULL, NULL) == RingInvalid);
    dmabuf_version = 3;
    assert(venus_present_create(&presenter, display, dmabuf, surface, done, NULL) == RingInvalid);
    dmabuf_version = 4;
    surface_version = 3;
    assert(venus_present_create(&presenter, display, dmabuf, surface, done, NULL) == RingInvalid);
    surface_version = 4;
    for (fault = 1; fault <= 3; fault++) {
        assert(venus_present_create(&presenter, display, dmabuf, surface, done, NULL) ==
               RingCorrupt);
        assert(!presenter && !proxies && !owners);
    }
    fault = 0;
    assert(venus_present_free(NULL) == RingOk);
    assert(venus_present_free(&presenter) == RingOk);
}
static void test_submit(void) {
    int fd = table_fd(); // Ownership test fixture; real compositor rejects non-DMA-BUF.
    venus_present_t *presenter = create();
    assert(submit(NULL, 1, fd) == RingInvalid);
    assert(venus_present_submit(presenter, 1, &layout, NULL, 1, &damage, 1) == RingInvalid);
    assert(submit(presenter, 0, fd) == RingInvalid);
    assert(venus_present_submit(presenter, 1, NULL, &fd, 1, &damage, 1) == RingInvalid);
    assert(venus_present_submit(presenter, 1, &layout, &fd, 2, &damage, 1) == RingInvalid);
    assert(venus_present_submit(presenter, 1, &layout, &fd, 1, NULL, 1) == RingInvalid);
    assert(submit(presenter, 1, -1) == RingInvalid);
    assert(submit(presenter, 1, fd) == RingAgain);
    ready(presenter);
    layout.modifier = 1;
    assert(submit(presenter, 1, fd) == RingAgain);
    layout.modifier = 0;
    for (fault = 4; fault <= 5; fault++) {
        assert(submit(presenter, 1, fd) == RingCorrupt);
        assert(!presenter->last_frame && !presenter->slots[0].params && proxies == 1);
    }
    fault = 0;
    assert(submit(presenter, 1, fd) == RingOk);
    assert(submit(presenter, 1, fd) == RingInvalid);
    assert(submit(presenter, 2, fd) == RingAgain);
    assert(venus_present_free(&presenter) == RingAgain && presenter);
    rejected_buffer(&presenter->slots[0], presenter->slots[0].params);
    assert(completed_status == RingInvalid && completed_frame == 1);
    assert(submit(presenter, 2, fd) == RingOk);
    import_slot(presenter, 0);
    assert(commits == 1 && damage_calls == 1 && completions == 1);
    assert(submit(presenter, 3, fd) == RingAgain);
    // Release before pacing: memory returned, next submission still waits.
    released_buffer(&presenter->slots[0], presenter->slots[0].buffer);
    assert(completed_status == RingOk && completed_frame == 2);
    assert(submit(presenter, 3, fd) == RingAgain);
    pace(presenter);
    // Pacing before release: fill all three compositor-owned slots.
    for (unsigned index = 0; index < 3; index++) {
        assert(submit(presenter, index + 3, fd) == RingOk);
        import_slot(presenter, index);
        pace(presenter);
    }
    assert(submit(presenter, 6, fd) == RingAgain);
    assert(venus_present_free(&presenter) == RingAgain);
    for (unsigned index = 0; index < 3; index++)
        released_buffer(&presenter->slots[index], presenter->slots[index].buffer);
    // Updated feedback without a new table is atomically accepted.
    formats(presenter);
    assert(submit(presenter, 6, fd) == RingOk);
    import_slot(presenter, 0);
    released_buffer(&presenter->slots[0], presenter->slots[0].buffer);
    end(&presenter); // Idle teardown destroys independent remaining frame callback.
    assert(fcntl(fd, F_GETFD) >= 0);
    close(fd);
}
static void test_proxy_failures(void) {
    int fd = table_fd();
    for (fault = 6; fault <= 8; fault++) {
        venus_present_t *presenter = create();
        ready(presenter);
        assert(submit(presenter, 1, fd) == RingOk);
        import_slot(presenter, 0);
        assert(presenter->failed == RingCorrupt && completed_status == RingCorrupt);
        assert(submit(presenter, 2, fd) == RingCorrupt);
        end(&presenter);
    }
    fault = 0;
    venus_present_t *presenter = create();
    ready(presenter);
    assert(submit(presenter, 1, fd) == RingOk);
    disconnected = EPIPE;
    end(&presenter); // Pending params abandoned after confirmed display error.
    assert(completed_status == RingClosed);
    disconnected = 0;
    presenter = create();
    ready(presenter);
    assert(submit(presenter, 1, fd) == RingOk);
    import_slot(presenter, 0);
    disconnected = EPIPE;
    end(&presenter); // Imported buffer and independent pacing callback abandoned.
    assert(completed_status == RingClosed);
    disconnected = 0;
    close(fd);
}
static void test_feedback_faults(void) {
    for (int mode = 0; mode < 8; mode++) {
        venus_present_t *presenter = create();
        int fd = table_fd();
        uint32_t bytes = mode == 0 ? 0 : mode == 1 ? 65537 : mode == 2 ? 17 : mode == 3 ? 32 : 16;
        fault = mode == 4 ? 9 : mode == 5 ? 10 : 0;
        if (mode == 6)
            presenter->failed = RingCorrupt;
        if (mode == 7) {
            close(fd);
            fd = -1;
        }
        feedback_table(presenter, presenter->feedback, fd, bytes);
        assert(presenter->failed == RingCorrupt);
        if (fd >= 0)
            assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        fault = 0;
        end(&presenter);
    }
    for (int mode = 0; mode < 15; mode++) {
        venus_present_t *presenter = create();
        int fd = table_fd();
        feedback_table(presenter, presenter->feedback, fd, 16);
        dev_t device = 1;
        unsigned char indices[2] = {0};
        struct wl_array device_array = {.data = &device, .size = sizeof(device)};
        struct wl_array formats_array = {.data = indices, .size = sizeof(indices)};
        switch (mode) {
        case 0:
            device_array.size = 0;
            feedback_main(presenter, presenter->feedback, &device_array);
            break;
        case 1:
            device_array.data = NULL;
            feedback_main(presenter, presenter->feedback, &device_array);
            break;
        case 2:
            feedback_main(presenter, presenter->feedback, &device_array);
            feedback_main(presenter, presenter->feedback, &device_array);
            break;
        case 3:
            device_array.size = 0;
            feedback_target(presenter, presenter->feedback, &device_array);
            break;
        case 4:
            device_array.data = NULL;
            feedback_target(presenter, presenter->feedback, &device_array);
            break;
        case 5:
            feedback_target(presenter, presenter->feedback, &device_array);
            feedback_target(presenter, presenter->feedback, &device_array);
            break;
        case 6:
            feedback_formats(presenter, presenter->feedback, &formats_array);
            break;
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
            feedback_target(presenter, presenter->feedback, &device_array);
            if (mode == 7)
                formats_array.data = NULL;
            if (mode == 8)
                formats_array.size = 8193;
            if (mode == 9)
                formats_array.size = 1;
            if (mode == 10)
                indices[0] = 1;
            feedback_formats(presenter, presenter->feedback, &formats_array);
            if (mode == 11)
                feedback_formats(presenter, presenter->feedback, &formats_array);
            break;
        case 12:
            feedback_tranche_done(presenter, presenter->feedback);
            break;
        case 13:
            feedback_target(presenter, presenter->feedback, &device_array);
            feedback_tranche_done(presenter, presenter->feedback);
            break;
        default:
            feedback_done(presenter, presenter->feedback);
            break;
        }
        assert(presenter->failed == RingCorrupt);
        feedback_done(presenter, presenter->feedback);
        end(&presenter);
    }
}
int main(void) {
    test_create();
    test_submit();
    test_proxy_failures();
    test_feedback_faults();
    int fd = table_fd();
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        venus_present_t *presenter = create();
        ready(presenter);
        assert(submit(presenter, 1, fd) == RingOk);
        import_slot(presenter, 0);
        pace(presenter);
        released_buffer(&presenter->slots[0], presenter->slots[0].buffer);
        end(&presenter);
    }
    close(fd);
    assert(!proxies && !owners && plane_calls && damage_calls && completions);
    return 0;
}
