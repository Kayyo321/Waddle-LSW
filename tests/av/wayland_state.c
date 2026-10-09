/** @file wayland_state.c @brief xdg-shell zero-size state transitions and delivery failure.
 * Includes the implementation to exercise private compositor callbacks without
 * fabricating a display connection; native platform tests cover real protocol I/O.
 */
#include <wayland-client.h>
#include <stdint.h>
#include <stdarg.h>
/* Native protocol double: production callbacks and message dispatch are real;
 * every Wayland request is counted before it could reach a compositor. */
static unsigned native_effects, proxy_count;
static uintptr_t proxy_storage[4096];
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)proxy; (void)opcode; (void)version; (void)flags;
    ++native_effects;
    if (!interface) return NULL;
    if (proxy_count >= 4096) __builtin_trap();
    return (struct wl_proxy *)&proxy_storage[proxy_count++];
}
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) { (void)proxy; return 1; }
int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    (void)proxy; (void)implementation; (void)data; return 0;
}
int __wrap_wl_display_flush(struct wl_display *display) { (void)display; return 0; }
#include "av_wayland.c"
#include <assert.h>
#include <fcntl.h>
static av_message_t delivered;
static unsigned deliveries;
static int fail_delivery;
static void done(void *context) { (void)context; }
static int receive_request(const av_message_t *message, void *context) {
    assert(context == &deliveries);
    delivered = *message;
    ++deliveries;
    return fail_delivery ? -1 : 0;
}
static void lifecycle_callbacks(void);
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
    assert(deliveries == 4); /* bounds apply to new downs and wheel during implicit grabs */
    pointer_button(&client, NULL, 0, 0, 0x110, 0);
    assert(deliveries == 5 && delivered.flags == 0); /* releases always escape */
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(320), wl_fixed_from_int(20));
    pointer_button(&client, NULL, 0, 0, 0x110, 1);
    assert(deliveries == 5);
    pointer_motion(&client, NULL, 0, wl_fixed_from_int(319), wl_fixed_from_int(199));
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    assert(deliveries == 7 && delivered.type == MsgInputWheel && delivered.y == -120);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(10));
    assert(deliveries == 8 && delivered.x == 120);
    pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, INT32_MAX);
    pointer_button(&client, NULL, 0, 0, 0x113, 1);
    assert(deliveries == 8);
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
    lifecycle_callbacks();
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
