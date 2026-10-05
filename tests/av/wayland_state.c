/** @file wayland_state.c @brief xdg-shell zero-size state transitions and delivery failure.
 * Includes the implementation to exercise private compositor callbacks without
 * fabricating a display connection; native platform tests cover real protocol I/O.
 */
#include "av_wayland.c"
#include <assert.h>
static av_message_t delivered;
static unsigned deliveries;
static int fail_delivery;
static int receive_request(const av_message_t *message, void *context) {
    assert(context == &deliveries);
    delivered = *message;
    ++deliveries;
    return fail_delivery ? -1 : 0;
}
int main(void) {
    av_wayland_t client = {.request = receive_request, .request_context = &deliveries};
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
    return 0;
}
