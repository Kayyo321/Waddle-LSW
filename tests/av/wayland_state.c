/** @file wayland_state.c @brief Real Wayland callbacks for V1/V2 lifecycle and V3 input lease integration.
 * Includes the implementation to exercise private compositor callbacks without
 * fabricating a display connection; native platform tests cover real protocol I/O.
 */
#include <wayland-client.h>
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
static unsigned proxy_index(struct wl_proxy *proxy) {
    uintptr_t address = (uintptr_t)proxy;
    uintptr_t base = (uintptr_t)proxy_storage;
    if (address < base || address >= base + sizeof(proxy_storage) ||
        (address - base) % sizeof(proxy_storage[0])) __builtin_trap();
    return (unsigned)((address - base) / sizeof(proxy_storage[0]));
}
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)proxy; (void)opcode; (void)version; (void)flags;
    ++native_effects;
    if (!interface) return NULL;
    int callback = !strcmp(interface->name, "wl_callback");
    if (callback) {
        ++syncs;
        if (sync_failure) return NULL;
    }
    if (proxy_count >= 4096) __builtin_trap();
    callback_proxies[proxy_count] = (uint8_t)callback;
    return (struct wl_proxy *)&proxy_storage[proxy_count++];
}
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) { (void)proxy; return 1; }
int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    unsigned index = proxy_index(proxy);
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
            if (wheel) pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
            else pointer_button(&client, NULL, 0, 0, 0x110, 1);
            assert(client.delivery_failed && client.lease.phase == AvLeaseHostTerminal);
            assert(deliveries == before + failing_frame && delivered_log[before].type == MsgInputPointerV3);
            pointer_axis(&client, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
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
