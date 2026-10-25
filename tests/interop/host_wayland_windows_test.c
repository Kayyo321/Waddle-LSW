/**
 * @file host_wayland_windows_test.c
 * @brief Tests of host_wayland_windows.c. The module source is included directly so its static
 * helpers are exercised; Wayland entry points are wrapped (see each section) so no display is needed.
 */
#include "../../src/interop/host_wayland_windows.c"

#include <stdio.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "host_wayland_windows_test.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* Sentinel pointers stand in for borrowed compositor objects in this part of the suite. */
static struct wl_compositor *const fake_compositor = (struct wl_compositor *)(void *)&fake_compositor;
static struct xdg_wm_base *const fake_wm_base = (struct xdg_wm_base *)(void *)&fake_wm_base;
static struct wl_seat *const fake_seat = (struct wl_seat *)(void *)&fake_seat;
static struct wl_seat *const other_seat = (struct wl_seat *)(void *)&other_seat;

static host_wayland_config_t valid_config(void) {
    host_wayland_config_t config;
    memset(&config, 0, sizeof config);
    config.compositor = fake_compositor;
    config.wm_base = fake_wm_base;
    config.wm_base_version = 5;
    config.device = "dev0";
    return config;
}

static int check_init(void) {
    static host_wayland_t ctx;
    host_wayland_config_t config = valid_config();
    Check(host_wayland_windows_init(NULL, &config) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_init(&ctx, NULL) == HostWaylandInvalidArgument);
    host_wayland_config_t bad = config;
    bad.compositor = NULL;
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.wm_base = NULL;
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.wm_base_version = 0;
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.device = "";
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.device = "0123456789012345678901234567890123456789012345678901234567890123x";
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.scale_numerator = 1;
    bad.scale_denominator = 0;
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);
    bad = config;
    bad.scale_numerator = 961;
    bad.scale_denominator = 120;
    Check(host_wayland_windows_init(&ctx, &bad) == HostWaylandInvalidArgument);

    Check(host_wayland_windows_init(&ctx, &config) == HostWaylandOk);
    Check(ctx.initialized == 1 && ctx.scale == 1);
    Check(ctx.config.scale_numerator == 1 && ctx.config.scale_denominator == 1);
    Check(host_wayland_windows_window_count(&ctx) == 0);
    Check(!host_wayland_windows_is_mapped(&ctx, 1, 1));
    return 0;
}

static int check_scale(void) {
    static host_wayland_t ctx;
    host_wayland_config_t config = valid_config();
    config.scale_numerator = 150;
    config.scale_denominator = 120;
    Check(host_wayland_windows_init(&ctx, &config) == HostWaylandOk);
    Check(ctx.scale == 2);
    Check(host_wayland_windows_set_scale(&ctx, 240, 120) == HostWaylandOk);
    Check(ctx.scale == 2);
    Check(host_wayland_windows_set_scale(&ctx, 1, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_set_scale(&ctx, 961, 120) == HostWaylandInvalidArgument);
    Check(ctx.scale == 2);
    Check(host_wayland_windows_set_scale(NULL, 1, 1) == HostWaylandNotInitialized);
    Check(ceil_div_scale(9, 8) == 2 && ceil_div_scale(0, 8) == 0);
    return 0;
}

static int check_presses(void) {
    static host_wayland_t ctx;
    host_wayland_config_t config = valid_config();
    Check(host_wayland_windows_init(&ctx, &config) == HostWaylandOk);

    Check(host_wayland_windows_record_press(&ctx, 0, 7, fake_seat, 1, 1, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_record_press(&ctx, 1, 7, NULL, 1, 1, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_record_press(&ctx, 1, 7, fake_seat, 0, 1, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_record_press(&ctx, 1, 7, fake_seat, 1, 0, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_record_press(NULL, 1, 7, fake_seat, 1, 1, 0) == HostWaylandNotInitialized);

    Check(host_wayland_windows_record_press(&ctx, 10, 7, fake_seat, 1, 1, 1000) == HostWaylandOk);
    host_wayland_press_t *press = press_find(&ctx, 10, 1000);
    Check(press != NULL && press->wayland_serial == 7 && press->seat == fake_seat && press->window == 1);
    /* Valid for exactly the 300 ms window, expired afterwards. */
    Check(press_find(&ctx, 10, 1300) != NULL);
    Check(press_find(&ctx, 10, 1301) == NULL);
    /* A clock that moved backwards keeps entries. */
    Check(host_wayland_windows_record_press(&ctx, 11, 8, other_seat, 2, 3, 2000) == HostWaylandOk);
    Check(press_find(&ctx, 11, 1500) != NULL);
    host_wayland_windows_release_press(&ctx, 11);
    Check(press_find(&ctx, 11, 2000) == NULL);
    host_wayland_windows_release_press(NULL, 11);
    host_wayland_windows_release_press(&ctx, 12);

    /* Fill all 64 entries live; the 65th is refused and no live entry is evicted. */
    for (uint32_t serial = 100; serial < 100 + PRESS_COUNT; ++serial) {
        Check(host_wayland_windows_record_press(&ctx, serial, serial, fake_seat, 5, 5, 5000) == HostWaylandOk);
    }
    Check(host_wayland_windows_record_press(&ctx, 999, 1, fake_seat, 5, 5, 5000) == HostWaylandPressTableFull);
    Check(press_find(&ctx, 100, 5000) != NULL && press_find(&ctx, 163, 5000) != NULL);
    /* Once the validity window has passed, the table makes room again. */
    Check(host_wayland_windows_record_press(&ctx, 999, 1, fake_seat, 5, 5, 5301) == HostWaylandOk);
    Check(press_find(&ctx, 100, 5301) == NULL);
    /* Releasing a live press frees its slot immediately. */
    host_wayland_windows_release_press(&ctx, 999);
    Check(host_wayland_windows_record_press(&ctx, 1000, 1, fake_seat, 5, 5, 5301) == HostWaylandOk);
    return 0;
}

int main(void) {
    if (check_init() != 0) return 1;
    if (check_scale() != 0) return 1;
    if (check_presses() != 0) return 1;
    puts("host_wayland_windows_test: all checks passed");
    return 0;
}
