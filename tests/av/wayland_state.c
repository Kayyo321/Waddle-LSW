/** @file wayland_state.c @brief xdg-shell zero-size state transitions and delivery failure.
 * Includes the implementation to exercise private compositor callbacks without
 * fabricating a display connection; native platform tests cover real protocol I/O.
 */
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
    uint8_t mapping[64] = {0};
    assert(av_wayland_watch(&client, 0, 1, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 0, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 0x1000000, mapping, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, NULL, sizeof(mapping), done, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), NULL, NULL) == -1);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), done, NULL) == 0);
    assert(av_wayland_watch(&client, 1, 1, mapping, sizeof(mapping), done, NULL) == -1);
    video_window_t window = {.client = &client, .geometry = {.window_id = 1,
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
    return 0;
}
