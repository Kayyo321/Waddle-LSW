/**
 * @file host_wayland_windows_test.c
 * @brief Tests of host_wayland_windows.c: press table, realization and events. The module source is included
 * directly so its static helpers are exercised, and the recording Wayland double (host_wayland_windows_fake.h)
 * replaces the generated request wrappers.
 */
#include "../../src/interop/host_wayland_windows.c"

#include "host_wayland_windows_fake.h"

#include <stdio.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "host_wayland_windows_test.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define DECORATION_CLIENT ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
#define DECORATION_SERVER ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE

/** @brief Session context shared by the tests; lives in static storage. */
static host_wayland_t session_ctx;

/* ---- callback capture -------------------------------------------------------------------------- */

typedef struct capture_t {
    unsigned stale_callbacks; /* callbacks for a window the table does not hold realized */
    unsigned configures, closes, repositioned, dismissed, errors;
    uint32_t last_bits, last_width, last_height, last_error_code;
    uint32_t last_popup_width, last_popup_height;
    int32_t last_x, last_y;
    uint16_t last_refused_type;
    uint64_t last_window, last_generation;
} capture_t;

static capture_t captured;

/** @brief Every success callback must name a window the table holds realized; anything else is a stale callback. */
static void check_current(uint64_t window, uint64_t generation) {
    host_window_ref_t id;
    id.window = window;
    id.generation = generation;
    if (!host_window_map_is_current(&session_ctx.map, id)) ++captured.stale_callbacks;
}

static void cb_configure(void *user, uint64_t window, uint64_t generation, uint32_t bits, uint32_t width, uint32_t height) {
    (void)user;
    check_current(window, generation);
    ++captured.configures;
    captured.last_window = window;
    captured.last_generation = generation;
    captured.last_bits = bits;
    captured.last_width = width;
    captured.last_height = height;
}

static void cb_close(void *user, uint64_t window, uint64_t generation) {
    (void)user;
    check_current(window, generation);
    ++captured.closes;
    captured.last_window = window;
    captured.last_generation = generation;
}

static void cb_repositioned(void *user, uint64_t window, uint64_t generation, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    (void)user;
    check_current(window, generation);
    ++captured.repositioned;
    captured.last_window = window;
    captured.last_generation = generation;
    captured.last_x = x;
    captured.last_y = y;
    captured.last_popup_width = width;
    captured.last_popup_height = height;
}

static void cb_dismissed(void *user, uint64_t window, uint64_t generation) {
    (void)user;
    check_current(window, generation);
    ++captured.dismissed;
}

static void cb_error(void *user, uint16_t refused_type, uint32_t error_code, uint64_t window, uint64_t generation) {
    (void)user;
    ++captured.errors;
    captured.last_refused_type = refused_type;
    captured.last_error_code = error_code;
    captured.last_window = window;
    captured.last_generation = generation;
}

static void capture_reset(void) {
    memset(&captured, 0, sizeof captured);
}

/* ---- fixtures ---------------------------------------------------------------------------------- */

/* Sentinel pointers stand in for borrowed compositor objects in the press-table and init tests. */
static struct wl_compositor *const sentinel_compositor = (struct wl_compositor *)(void *)&sentinel_compositor;
static struct xdg_wm_base *const sentinel_wm_base = (struct xdg_wm_base *)(void *)&sentinel_wm_base;
static struct wl_seat *const fake_seat = (struct wl_seat *)(void *)&fake_seat;
static struct wl_seat *const other_seat = (struct wl_seat *)(void *)&other_seat;

static host_wayland_config_t sentinel_config(void) {
    host_wayland_config_t config;
    memset(&config, 0, sizeof config);
    config.compositor = sentinel_compositor;
    config.wm_base = sentinel_wm_base;
    config.wm_base_version = 5;
    config.device = "dev0";
    config.callbacks.on_configure = cb_configure;
    config.callbacks.on_close = cb_close;
    config.callbacks.on_error = cb_error;
    return config;
}

/** @brief Configuration backed by fake proxies, so realization issues recorded requests. */
static host_wayland_config_t fake_config(const char *device, uint32_t wm_base_version) {
    host_wayland_config_t config = sentinel_config();
    config.compositor = (struct wl_compositor *)fake_object(fake_new("wl_compositor"));
    config.wm_base = (struct xdg_wm_base *)fake_object(fake_new("xdg_wm_base"));
    config.wm_base_version = wm_base_version;
    config.device = device;
    config.callbacks.on_popup_repositioned = cb_repositioned;
    config.callbacks.on_popup_dismissed = cb_dismissed;
    return config;
}

/* ---- frame builder: real codec writer ------------------------------------------------------------ */

typedef struct announce_spec_t {
    uint64_t window;
    uint64_t generation;
    bool has_parent;
    uint64_t parent;
    uint64_t parent_generation;
    uint32_t role;
    int32_t x, y;
    uint32_t width, height;
    const char *title; /* NULL omits the title */
    const char *app_key;
    bool has_min_size;
    uint32_t min_width, min_height;
    uint32_t state;
    uint32_t dpi;
    bool has_serial;
    uint32_t serial;
} announce_spec_t;

static size_t build_announce(uint8_t *frame, size_t capacity, const announce_spec_t *spec) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    interop_put_uint(&writer, InteropTagWindow, spec->window);
    interop_put_uint(&writer, InteropTagGeneration, spec->generation);
    if (spec->has_parent) {
        interop_put_uint(&writer, InteropTagParent, spec->parent);
        interop_put_uint(&writer, InteropTagParentGeneration, spec->parent_generation);
    }
    interop_put_uint(&writer, InteropTagRole, spec->role);
    interop_put_rect(&writer, InteropTagRect, spec->x, spec->y, spec->width, spec->height);
    if (spec->title) interop_put_bytes(&writer, InteropTagTitle, spec->title, (uint16_t)strlen(spec->title));
    interop_put_bytes(&writer, InteropTagAppKey, spec->app_key, (uint16_t)strlen(spec->app_key));
    interop_put_uint(&writer, InteropTagState, spec->state);
    interop_put_uint(&writer, InteropTagDpi, spec->dpi);
    if (spec->has_serial) interop_put_uint(&writer, InteropTagSerial, spec->serial);
    if (spec->has_min_size) {
        uint8_t minimum[8];
        for (unsigned byte = 0; byte < 4; ++byte) {
            minimum[byte] = (uint8_t)(spec->min_width >> (8 * byte));
            minimum[4 + byte] = (uint8_t)(spec->min_height >> (8 * byte));
        }
        interop_put_bytes(&writer, InteropTagMinSize, minimum, sizeof minimum);
    }
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowAnnounce, 0, InteropFromGuest, &length) != InteropErrorNone) return 0;
    return length;
}

static announce_spec_t toplevel_spec(uint64_t window, uint64_t generation, const char *app_key) {
    announce_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.window = window;
    spec.generation = generation;
    spec.role = 1;
    spec.x = 10;
    spec.y = 20;
    spec.width = 640;
    spec.height = 480;
    spec.app_key = app_key;
    spec.dpi = 96;
    return spec;
}

/** @brief Announce one frame built from `spec`; returns the module status. */
static host_wayland_status_t announce_spec(host_wayland_t *ctx, const announce_spec_t *spec) {
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, spec);
    if (length == 0) return HostWaylandMalformedFrame;
    return host_wayland_windows_handle_frame(ctx, frame, length, 1000);
}

/* ---- press table and configuration (from the first slice) ----------------------------------------- */

static int check_init(void) {
    static host_wayland_t ctx;
    host_wayland_config_t config = sentinel_config();
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
    host_wayland_config_t config = sentinel_config();
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
    host_wayland_config_t config = sentinel_config();
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

/* ---- realization: toplevels --------------------------------------------------------------------- */


/* Proxies the fixture creates for compositor and wm_base are borrowed by the module and never destroyed by
 * it; leak accounting excludes them. */
static unsigned fixture_creates;

static int begin_session(const char *device) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config(device, 5);
    fixture_creates = fake_creates;
    return host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk ? 0 : 1;
}

static int check_toplevel_realization(void) {
    Check(begin_session("dev0") == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    spec.title = "Title";
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);

    unsigned surface = fake_nth("wl_surface", 0);
    unsigned xdg_surface = fake_nth("xdg_surface", 0);
    unsigned toplevel = fake_nth("xdg_toplevel", 0);
    Check(surface != FAKE_NONE && xdg_surface != FAKE_NONE && toplevel != FAKE_NONE);
    Check(fake_count_requests("wl_compositor", WL_COMPOSITOR_CREATE_SURFACE) == 1);
    int get_surface = fake_find("xdg_wm_base", XDG_WM_BASE_GET_XDG_SURFACE, 0);
    Check(get_surface >= 0 && fake_requests[get_surface].a0 == (long long)surface);
    Check(fake_proxy_of_first("xdg_surface", XDG_SURFACE_GET_TOPLEVEL, 0) == xdg_surface);
    int app = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, 0);
    Check(app >= 0 && fake_requests[app].proxy == toplevel && !strcmp(fake_requests[app].text, "waddle.dev0.a-app"));
    int title = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE, 0);
    Check(title >= 0 && !strcmp(fake_requests[title].text, "Title"));
    int commit = fake_find("wl_surface", WL_SURFACE_COMMIT, 0);
    Check(commit >= 0 && fake_requests[commit].proxy == surface);
    /* app_id and title are sent before the first commit; the initial commit carries no buffer. */
    Check(app < commit && title < commit);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 0);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    Check(host_wayland_windows_window_count(&session_ctx) == 1);

    /* The first configure is acknowledged and only then does the window become mapped. */
    deliver_surface_configure(xdg_surface, 9);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 1);
    int ack = fake_find("xdg_surface", XDG_SURFACE_ACK_CONFIGURE, 0);
    Check(ack >= 0 && fake_requests[ack].a0 == 9);
    Check(host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    Check(!host_wayland_windows_is_mapped(&session_ctx, 1, 2));
    return 0;
}

static int check_uac_and_dialog(void) {
    Check(begin_session("dev0") == 0);
    announce_spec_t uac = toplevel_spec(1, 1, "a-ignored");
    uac.role = 5;
    Check(announce_spec(&session_ctx, &uac) == HostWaylandOk);
    int app = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, 0);
    Check(app >= 0 && !strcmp(fake_requests[app].text, "waddle.dev0.uac"));
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE) == 0);

    Check(begin_session("dev0") == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t dialog = toplevel_spec(2, 1, "a-child");
    dialog.role = 2;
    dialog.has_parent = true;
    dialog.parent = 1;
    dialog.parent_generation = 1;
    Check(announce_spec(&session_ctx, &dialog) == HostWaylandOk);
    unsigned root_toplevel = fake_nth("xdg_toplevel", 0);
    unsigned dialog_toplevel = fake_nth("xdg_toplevel", 1);
    int parent_request = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_PARENT, 0);
    Check(parent_request >= 0 && fake_requests[parent_request].proxy == dialog_toplevel);
    Check(fake_requests[parent_request].a0 == (long long)root_toplevel);

    /* A toplevel never names a parent: the table refuses it and nothing is sent. */
    Check(begin_session("dev0") == 0);
    announce_spec_t bad = toplevel_spec(3, 1, "a-x");
    bad.has_parent = true;
    bad.parent = 1;
    bad.parent_generation = 1;
    Check(announce_spec(&session_ctx, &bad) == HostWaylandRefused);
    Check(fake_request_count == 0);
    return 0;
}

static int check_min_size(void) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", 5);
    config.scale_numerator = 2;
    config.scale_denominator = 1;
    Check(host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    spec.has_min_size = true;
    spec.min_width = 1000;
    spec.min_height = 501;
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    int minimum = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_MIN_SIZE, 0);
    Check(minimum >= 0 && fake_requests[minimum].a0 == 500 && fake_requests[minimum].a1 == 251);
    return 0;
}

static int check_configure_and_close(void) {
    Check(begin_session("dev0") == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned toplevel = fake_nth("xdg_toplevel", 0);
    /* Maximized, fullscreen and activated map onto state bits; minimized never reaches the guest. */
    const uint32_t states[] = {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED, XDG_TOPLEVEL_STATE_FULLSCREEN, 3u};
    deliver_toplevel_configure(toplevel, 800, -5, states, 4);
    Check(captured.configures == 1);
    Check(captured.last_bits == (InteropStateMaximized | InteropStateActivated | InteropStateFullscreen));
    Check(captured.last_width == 800 && captured.last_height == 0);
    Check(captured.last_window == 1 && captured.last_generation == 1);
    deliver_toplevel_configure(toplevel, 0, 0, NULL, 0);
    Check(captured.configures == 2 && captured.last_bits == 0);
    deliver_toplevel_close(toplevel);
    Check(captured.closes == 1);
    return 0;
}

static int check_refusals(void) {
    Check(begin_session("dev0") == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    unsigned requests_before = fake_request_count;

    /* Duplicate: a protocol violation, nothing sent. */
    Check(announce_spec(&session_ctx, &root) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorProtocolViolation && captured.last_refused_type == 0x0100);
    /* Same window under another generation: a stale-window conflict. */
    announce_spec_t other = toplevel_spec(1, 2, "a-root");
    Check(announce_spec(&session_ctx, &other) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorStaleWindow);
    /* A dialog whose parent was never announced is a stale parent. */
    announce_spec_t dialog = toplevel_spec(2, 1, "a-child");
    dialog.role = 2;
    dialog.has_parent = true;
    dialog.parent = 9;
    dialog.parent_generation = 1;
    Check(announce_spec(&session_ctx, &dialog) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorStaleWindow && captured.last_window == 2);
    Check(captured.errors == 3);
    Check(fake_request_count == requests_before);
    Check(host_wayland_windows_window_count(&session_ctx) == 1);
    /* A popup under a parent that was never announced is refused with StaleWindow, and nothing is sent. */
    announce_spec_t menu = toplevel_spec(3, 1, "a-menu");
    menu.role = 3;
    menu.has_parent = true;
    menu.parent = 9;
    menu.parent_generation = 1;
    Check(announce_spec(&session_ctx, &menu) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorStaleWindow && captured.last_window == 3);
    Check(fake_request_count == requests_before);
    return 0;
}

/** @brief Expect a realization that fails at one object: rollback, error, no leak, child-first destroy. */
static int expect_rollback(const char *fail_iface, const char *listener_iface) {
    Check(begin_session("dev0") == 0);
    if (fail_iface) fake_fail_create = fail_iface;
    if (listener_iface) fake_fail_listener = listener_iface;
    announce_spec_t spec = toplevel_spec(7, 3, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandRealizeFailed);
    Check(captured.errors == 1 && captured.last_refused_type == 0x0100 && captured.last_error_code == InteropErrorUnexpected);
    Check(captured.last_window == 7 && captured.last_generation == 3);
    Check(host_wayland_windows_window_count(&session_ctx) == 0);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 7, 3));
    Check(fake_creates - fixture_creates == fake_destroys && fake_double_destroys == 0);
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].iface && (!strcmp(fake_proxies[index].iface, "wl_compositor") || !strcmp(fake_proxies[index].iface, "xdg_wm_base"))) continue;
        Check(!fake_proxies[index].live);
    }

    /* Destroys are child first: xdg_toplevel before xdg_surface before wl_surface. */
    int surface_destroy = -1, xdg_destroy = -1;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        if (!fake_requests[index].destroy || !fake_requests[index].iface) continue;
        if (!strcmp(fake_requests[index].iface, "xdg_surface")) xdg_destroy = (int)index;
        if (!strcmp(fake_requests[index].iface, "wl_surface")) surface_destroy = (int)index;
    }
    if (xdg_destroy >= 0 && surface_destroy >= 0) Check(xdg_destroy < surface_destroy);

    /* The rolled-back window left no residue: the same identity realizes once the fault is gone. */
    fake_fail_create = NULL;
    fake_fail_listener = NULL;
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    Check(host_wayland_windows_window_count(&session_ctx) == 1);
    return 0;
}

static int check_rollback_and_stale(void) {
    /* Each creation and listener point rolls back the window; the step order is surface, xdg_surface, toplevel. */
    Check(expect_rollback("wl_surface", NULL) == 0);
    Check(expect_rollback("xdg_surface", NULL) == 0);
    Check(expect_rollback("xdg_toplevel", NULL) == 0);
    Check(expect_rollback(NULL, "xdg_surface") == 0);
    Check(expect_rollback(NULL, "xdg_toplevel") == 0);

    /* A late configure on the rolled-back surface is a no-op: no ack, no callback. */
    Check(begin_session("dev0") == 0);
    fake_fail_create = "xdg_toplevel";
    announce_spec_t spec = toplevel_spec(7, 3, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandRealizeFailed);
    fake_fail_create = NULL;
    unsigned dead_surface = fake_nth("xdg_surface", 0);
    Check(dead_surface != FAKE_NONE);
    deliver_surface_configure(dead_surface, 4);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 0);
    return 0;
}

static int check_malformed_and_lifecycle(void) {
    host_wayland_t uninitialized;
    memset(&uninitialized, 0, sizeof uninitialized);
    uint8_t frame[64];
    memset(frame, 0, sizeof frame);
    Check(host_wayland_windows_handle_frame(NULL, frame, sizeof frame, 0) == HostWaylandInvalidArgument);
    Check(host_wayland_windows_handle_frame(&uninitialized, frame, sizeof frame, 0) == HostWaylandNotInitialized);

    Check(begin_session("dev0") == 0);
    Check(host_wayland_windows_handle_frame(&session_ctx, NULL, 16, 0) == HostWaylandInvalidArgument);
    memset(frame, 0, sizeof frame);
    Check(host_wayland_windows_handle_frame(&session_ctx, frame, sizeof frame, 0) == HostWaylandMalformedFrame);
    /* A valid announce cut short is malformed, not realized. */
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    uint8_t valid[InteropMaxFrameBytes];
    size_t length = build_announce(valid, sizeof valid, &spec);
    Check(length > InteropHeaderBytes);
    Check(host_wayland_windows_handle_frame(&session_ctx, valid, length - 4, 0) == HostWaylandMalformedFrame);
    Check(fake_request_count == 0);
    /* Frames this layer does not realize yet are reported as unsupported types, not refused. */
    Check(host_wayland_windows_window_count(&session_ctx) == 0);
    return 0;
}


/* ---- decorations (2.6) --------------------------------------------------------------------------- */

/** @brief Session whose config carries a decoration manager fake. */
static int begin_decorated_session(void) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", 5);
    config.decoration_manager = (struct zxdg_decoration_manager_v1 *)fake_object(fake_new("zxdg_decoration_manager_v1"));
    fixture_creates = fake_creates;
    return host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk ? 0 : 1;
}

/** @brief Index of the nth destroy request on `iface`, or -1. */
static int fake_find_destroy(const char *iface, unsigned nth) {
    for (unsigned index = 0; index < fake_request_count; ++index) {
        if (fake_requests[index].destroy && fake_requests[index].iface && !strcmp(fake_requests[index].iface, iface)) {
            if (nth == 0) return (int)index;
            --nth;
        }
    }
    return -1;
}

static int check_decoration_request(void) {
    Check(begin_decorated_session() == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned toplevel = fake_nth("xdg_toplevel", 0);
    unsigned decoration = fake_nth("zxdg_toplevel_decoration_v1", 0);
    Check(decoration != FAKE_NONE);
    int get = fake_find("zxdg_decoration_manager_v1", ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION, 0);
    Check(get >= 0 && fake_requests[get].a0 == (long long)toplevel);
    int mode = fake_find("zxdg_toplevel_decoration_v1", ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE, 0);
    Check(mode >= 0 && fake_requests[mode].proxy == decoration && fake_requests[mode].a0 == DECORATION_CLIENT);
    /* Client-side mode is requested before the first commit. */
    int commit = fake_find("wl_surface", WL_SURFACE_COMMIT, 0);
    Check(mode < commit);

    /* Dialogs and consent dialogs are decorated too. */
    announce_spec_t uac = toplevel_spec(2, 1, "a-consent");
    uac.role = 5;
    Check(announce_spec(&session_ctx, &uac) == HostWaylandOk);
    Check(fake_count_requests("zxdg_decoration_manager_v1", ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) == 2);

    /* Without the manager no decoration is requested and nothing fails. */
    Check(begin_session("dev0") == 0);
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    Check(fake_nth("zxdg_toplevel_decoration_v1", 0) == FAKE_NONE);
    return 0;
}

static int check_decoration_staging(void) {
    Check(begin_decorated_session() == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned decoration = fake_nth("zxdg_toplevel_decoration_v1", 0);
    unsigned xdg_surface = fake_nth("xdg_surface", 0);

    /* The first configuration without a staged mode is acknowledged but keeps the window unmapped. */
    deliver_surface_configure(xdg_surface, 4);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 1);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    /* A staged mode is applied by the next surface configuration, not by the decoration event itself. */
    deliver_decoration_configure(decoration, DECORATION_CLIENT);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    deliver_surface_configure(xdg_surface, 5);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 2);
    Check(host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    /* Client-side mode is kept: the decoration is not destroyed and later configurations reuse it. */
    deliver_surface_configure(xdg_surface, 6);
    Check(host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    Check(fake_find_destroy("zxdg_toplevel_decoration_v1", 0) < 0);
    return 0;
}

static int check_server_side_fallback(void) {
    Check(begin_decorated_session() == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned decoration = fake_nth("zxdg_toplevel_decoration_v1", 0);
    unsigned xdg_surface = fake_nth("xdg_surface", 0);
    deliver_decoration_configure(decoration, DECORATION_SERVER);
    deliver_surface_configure(xdg_surface, 6);
    /* Acknowledge first, then destroy the decoration; the window is mapped with its guest frame. */
    int ack = fake_find("xdg_surface", XDG_SURFACE_ACK_CONFIGURE, 0);
    int destroy = fake_find_destroy("zxdg_toplevel_decoration_v1", 0);
    Check(ack >= 0 && fake_requests[ack].a0 == 6);
    Check(destroy > ack);
    Check(host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    /* The destroyed decoration's late events are no-ops. */
    deliver_decoration_configure(decoration, DECORATION_CLIENT);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 1);
    deliver_surface_configure(xdg_surface, 7);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 2);
    Check(fake_find_destroy("zxdg_toplevel_decoration_v1", 1) < 0);
    return 0;
}

static int check_invalid_mode(void) {
    Check(begin_decorated_session() == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned decoration = fake_nth("zxdg_toplevel_decoration_v1", 0);
    unsigned xdg_surface = fake_nth("xdg_surface", 0);
    /* A mode other than client or server terminates delivery: reported, and the window never maps. */
    deliver_decoration_configure(decoration, 7);
    Check(captured.errors == 1 && captured.last_error_code == InteropErrorProtocolViolation);
    deliver_decoration_configure(decoration, DECORATION_CLIENT);
    deliver_surface_configure(xdg_surface, 8);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 1);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 1, 1));
    return 0;
}

static int check_decoration_failures(void) {
    /* Creation failure: no decoration object exists, the window is rolled back and reported. */
    Check(begin_decorated_session() == 0);
    fake_fail_create = "zxdg_toplevel_decoration_v1";
    announce_spec_t spec = toplevel_spec(7, 3, "a-app");
    Check(announce_spec(&session_ctx, &spec) == HostWaylandRealizeFailed);
    Check(captured.errors == 1 && captured.last_error_code == InteropErrorUnexpected);
    Check(host_wayland_windows_window_count(&session_ctx) == 0);
    Check(fake_creates - fixture_creates == fake_destroys);

    /* Listener failure: the decoration exists and is destroyed before its toplevel (2.6). */
    Check(begin_decorated_session() == 0);
    fake_fail_listener = "zxdg_toplevel_decoration_v1";
    Check(announce_spec(&session_ctx, &spec) == HostWaylandRealizeFailed);
    int decoration_destroy = fake_find_destroy("zxdg_toplevel_decoration_v1", 0);
    int toplevel_destroy = fake_find_destroy("xdg_toplevel", 0);
    Check(decoration_destroy >= 0 && toplevel_destroy > decoration_destroy);
    Check(fake_creates - fixture_creates == fake_destroys && fake_double_destroys == 0);
    Check(host_wayland_windows_window_count(&session_ctx) == 0);
    return 0;
}


/* ---- popups (8.44, 3.6.2) ------------------------------------------------------------------------ */

/** @brief Toplevel 1 (640x480 guest pixels) and a Menu 3 under it at scale 1. */
static int begin_popup_session(uint32_t wm_base_version) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", wm_base_version);
    config.decoration_manager = (struct zxdg_decoration_manager_v1 *)fake_object(fake_new("zxdg_decoration_manager_v1"));
    fixture_creates = fake_creates;
    Check(host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk);
    return 0;
}

static announce_spec_t menu_spec(uint64_t window, uint64_t parent, uint64_t parent_generation, uint32_t role, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    announce_spec_t spec = toplevel_spec(window, 1, "a-menu");
    spec.role = role;
    spec.has_parent = true;
    spec.parent = parent;
    spec.parent_generation = parent_generation;
    spec.x = x;
    spec.y = y;
    spec.width = width;
    spec.height = height;
    return spec;
}

static int check_popup_realization(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    unsigned requests_before = fake_request_count;
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);

    unsigned popup = fake_nth("xdg_popup", 0);
    unsigned popup_surface = fake_nth("xdg_surface", 1);
    unsigned root_surface = fake_nth("xdg_surface", 0);
    Check(popup != FAKE_NONE && popup_surface != FAKE_NONE);
    /* The positioner carries the pure positioner values: parent 640 by 480, anchor clamped inside it. */
    unsigned positioner = fake_nth("xdg_positioner", 0);
    int size = fake_find("xdg_positioner", XDG_POSITIONER_SET_SIZE, requests_before);
    Check(size >= 0 && fake_requests[size].proxy == positioner && fake_requests[size].a0 == 40 && fake_requests[size].a1 == 20);
    int anchor = fake_find("xdg_positioner", XDG_POSITIONER_SET_ANCHOR_RECT, requests_before);
    Check(anchor >= 0 && fake_requests[anchor].a0 == 30 && fake_requests[anchor].a1 == 12 && fake_requests[anchor].a2 == 1 && fake_requests[anchor].a3 == 1);
    int gravity = fake_find("xdg_positioner", XDG_POSITIONER_SET_GRAVITY, requests_before);
    int anchor_value = fake_find("xdg_positioner", XDG_POSITIONER_SET_ANCHOR, requests_before);
    int constraint = fake_find("xdg_positioner", XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT, requests_before);
    Check(gravity >= 0 && fake_requests[gravity].a0 == 8);
    Check(anchor_value >= 0 && fake_requests[anchor_value].a0 == 5);
    Check(constraint >= 0 && fake_requests[constraint].a0 == 15);
    int offset = fake_find("xdg_positioner", XDG_POSITIONER_SET_OFFSET, requests_before);
    Check(offset >= 0 && fake_requests[offset].a0 == 0 && fake_requests[offset].a1 == 0);
    Check(fake_find("xdg_positioner", XDG_POSITIONER_SET_REACTIVE, requests_before) >= 0);

    /* The popup hangs under the direct parent's xdg_surface; the positioner goes away after get_popup. */
    int get_popup = fake_find("xdg_surface", XDG_SURFACE_GET_POPUP, requests_before);
    Check(get_popup >= 0 && fake_requests[get_popup].proxy == popup_surface);
    Check(fake_requests[get_popup].a0 == (long long)root_surface && fake_requests[get_popup].a1 == (long long)positioner);
    int positioner_destroy = fake_find("xdg_positioner", XDG_POSITIONER_DESTROY, requests_before);
    Check(positioner_destroy > get_popup);
    int commit = fake_find("wl_surface", WL_SURFACE_COMMIT, requests_before);
    Check(commit > positioner_destroy);
    /* Popups never get a decoration, and they are mapped only after their configure is acknowledged. */
    Check(fake_count_requests("zxdg_decoration_manager_v1", ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) == 1);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 3, 1));
    deliver_surface_configure(popup_surface, 11);
    Check(host_wayland_windows_is_mapped(&session_ctx, 3, 1));
    return 0;
}

static int check_popup_positioner_versions(void) {
    /* Protocol version 2 has no set_reactive. */
    Check(begin_popup_session(2) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    Check(fake_find("xdg_positioner", XDG_POSITIONER_SET_REACTIVE, 0) < 0);
    return 0;
}

/** @brief Trigger a Menu with protocol serial T; returns the module status. */
static host_wayland_status_t menu_with_serial(uint32_t serial, uint64_t window, uint64_t now_ms) {
    announce_spec_t menu = menu_spec(window, 1, 1, 3, 30, 12, 40, 20);
    menu.has_serial = true;
    menu.serial = serial;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, &menu);
    if (length == 0) return HostWaylandMalformedFrame;
    return host_wayland_windows_handle_frame(&session_ctx, frame, length, now_ms);
}

static int check_popup_grab(void) {
    /* A live press on the trigger's family grants the grab, sent before the first commit. */
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    unsigned seat = fake_new("wl_seat");
    struct wl_seat *seat_proxy = (struct wl_seat *)fake_object(seat);
    Check(host_wayland_windows_record_press(&session_ctx, 77, 500, seat_proxy, 1, 1, 1000) == HostWaylandOk);
    unsigned before = fake_request_count;
    Check(menu_with_serial(77, 3, 1100) == HostWaylandOk);
    int grab = fake_find("xdg_popup", XDG_POPUP_GRAB, before);
    int commit = fake_find("wl_surface", WL_SURFACE_COMMIT, before);
    Check(grab >= 0 && fake_requests[grab].a0 == (long long)seat && fake_requests[grab].a1 == 500);
    Check(grab < commit);
    host_wayland_windows_release_press(&session_ctx, 77);

    /* No entry, an expired entry, a stale serial, a non-menu role and a press outside the family: no grab. */
    Check(begin_popup_session(5) == 0);
    root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    before = fake_request_count;
    Check(menu_with_serial(78, 3, 1100) == HostWaylandOk);
    Check(fake_find("xdg_popup", XDG_POPUP_GRAB, before) < 0);

    Check(begin_popup_session(5) == 0);
    root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    seat = fake_new("wl_seat");
    seat_proxy = (struct wl_seat *)fake_object(seat);
    Check(host_wayland_windows_record_press(&session_ctx, 79, 501, seat_proxy, 1, 1, 1000) == HostWaylandOk);
    before = fake_request_count;
    Check(menu_with_serial(79, 3, 1301) == HostWaylandOk);
    Check(fake_find("xdg_popup", XDG_POPUP_GRAB, before) < 0);

    Check(begin_popup_session(5) == 0);
    root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    Check(host_wayland_windows_record_press(&session_ctx, 80, 502, seat_proxy, 9, 9, 1000) == HostWaylandOk);
    before = fake_request_count;
    Check(menu_with_serial(80, 3, 1100) == HostWaylandOk);
    Check(fake_find("xdg_popup", XDG_POPUP_GRAB, before) < 0);

    Check(begin_popup_session(5) == 0);
    root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    Check(host_wayland_windows_record_press(&session_ctx, 81, 503, seat_proxy, 1, 1, 1000) == HostWaylandOk);
    /* The codec allows a trigger serial on a Menu only, so a tooltip carrying one is malformed. */
    announce_spec_t tooltip = menu_spec(3, 1, 1, 4, 30, 12, 40, 20);
    tooltip.has_serial = true;
    tooltip.serial = 81;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, &tooltip);
    Check(length == 0 || host_wayland_windows_handle_frame(&session_ctx, frame, length, 1100) == HostWaylandMalformedFrame);
    /* Without a serial a tooltip is never granted a grab, even with a live press on its family. */
    tooltip.has_serial = false;
    Check(announce_spec(&session_ctx, &tooltip) == HostWaylandOk);
    Check(fake_count_requests("xdg_popup", XDG_POPUP_GRAB) == 0);
    return 0;
}

static int check_nested_popup(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    /* A 300 by 200 menu; the tooltip under it is clamped against the menu, not the toplevel. */
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 40, 40, 300, 200);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned before = fake_request_count;
    announce_spec_t tooltip = menu_spec(4, 3, 1, 4, 40, 450, 40, 40);
    Check(announce_spec(&session_ctx, &tooltip) == HostWaylandOk);
    unsigned menu_surface = fake_nth("xdg_surface", 1);
    int get_popup = fake_find("xdg_surface", XDG_SURFACE_GET_POPUP, before);
    Check(get_popup >= 0 && fake_requests[get_popup].a0 == (long long)menu_surface);
    int anchor = fake_find("xdg_positioner", XDG_POSITIONER_SET_ANCHOR_RECT, before);
    Check(anchor >= 0 && fake_requests[anchor].a0 == 40 && fake_requests[anchor].a1 == 199);
    int offset = fake_find("xdg_positioner", XDG_POSITIONER_SET_OFFSET, before);
    Check(offset >= 0 && fake_requests[offset].a0 == 0 && fake_requests[offset].a1 == 251);
    return 0;
}

static int check_popup_reposition_and_dismiss(void) {
    /* Scale 2: the origin (15, 6) is anchored as is; compositor movement maps back to guest pixels. */
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", 5);
    config.scale_numerator = 2;
    config.scale_denominator = 1;
    Check(host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    root.width = 1920;
    root.height = 1080;
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned popup = fake_nth("xdg_popup", 0);
    unsigned popup_surface = fake_nth("xdg_surface", 1);

    deliver_popup_configure(popup, 15, 6, 40, 20);
    Check(captured.repositioned == 1);
    Check(captured.last_x == 30 && captured.last_y == 12 && captured.last_popup_width == 40 && captured.last_popup_height == 20);
    deliver_popup_configure(popup, 13, 6, 40, 20);
    Check(captured.repositioned == 2 && captured.last_x == 26 && captured.last_y == 12);
    Check(captured.last_window == 3 && captured.last_generation == 1);

    /* popup_done marks the popup dismissed; it stays realized, configures are ignored, and no ack is sent. */
    deliver_popup_done(popup);
    Check(captured.dismissed == 1);
    Check(host_wayland_windows_window_count(&session_ctx) == 2);
    host_window_entry_t entry;
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){3, 1}, &entry) && entry.dismissed == 1);
    deliver_popup_configure(popup, 15, 6, 40, 20);
    Check(captured.repositioned == 2);
    deliver_surface_configure(popup_surface, 4);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 0);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 3, 1));
    deliver_popup_done(popup);
    Check(captured.dismissed == 1);
    return 0;
}

static int expect_popup_rollback(const char *fail_iface, const char *listener_iface) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    if (fail_iface) fake_fail_create = fail_iface;
    if (listener_iface) fake_fail_listener = listener_iface;
    unsigned creates = fake_creates, destroys = fake_destroys;
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandRealizeFailed);
    Check(host_wayland_windows_window_count(&session_ctx) == 1);
    Check(!host_wayland_windows_is_mapped(&session_ctx, 3, 1));
    /* Everything the failed popup created was destroyed again: the create and destroy deltas match. */
    Check((fake_creates - creates) == (fake_destroys - destroys) && fake_double_destroys == 0);
    Check(captured.last_error_code == InteropErrorUnexpected && captured.last_window == 3);
    fake_fail_create = NULL;
    fake_fail_listener = NULL;
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    return 0;
}

static int check_popup_refusals_and_rollback(void) {
    /* A popup whose geometry the positioner refuses is reported as MalformedFrame and rolled back. */
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    unsigned creates = fake_creates, destroys = fake_destroys;
    announce_spec_t huge = menu_spec(3, 1, 1, 3, 40000, 12, 40, 20);
    Check(announce_spec(&session_ctx, &huge) == HostWaylandRealizeFailed);
    Check(captured.last_error_code == InteropErrorMalformedFrame && captured.last_window == 3);
    Check(host_wayland_windows_window_count(&session_ctx) == 1);
    /* The positioner refuses before any object is created, so the failed announce creates and destroys nothing. */
    Check(fake_creates == creates && fake_destroys == destroys);

    /* Creation and listener failures roll the popup back like a toplevel, with its positioner destroyed. */
    Check(expect_popup_rollback("xdg_positioner", NULL) == 0);
    Check(expect_popup_rollback("xdg_popup", NULL) == 0);
    Check(expect_popup_rollback(NULL, "xdg_popup") == 0);
    Check(expect_popup_rollback("wl_surface", NULL) == 0);
    return 0;
}

/* ---- update, retire and reset (3.2.8, 4.3, 4.7, 8.44) ------------------------------------------------ */

typedef struct update_spec_t {
    uint64_t window;
    uint64_t generation;
    bool has_parent;
    uint64_t parent;
    uint64_t parent_generation;
    const char *title;
    bool has_min_size;
    uint32_t min_width;
    uint32_t min_height;
} update_spec_t;

static size_t build_update(uint8_t *frame, size_t capacity, const update_spec_t *spec) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    interop_put_uint(&writer, InteropTagWindow, spec->window);
    interop_put_uint(&writer, InteropTagGeneration, spec->generation);
    if (spec->has_parent) {
        interop_put_uint(&writer, InteropTagParent, spec->parent);
        interop_put_uint(&writer, InteropTagParentGeneration, spec->parent_generation);
    }
    interop_put_rect(&writer, InteropTagRect, 10, 20, 640, 480);
    if (spec->title) interop_put_bytes(&writer, InteropTagTitle, spec->title, (uint16_t)strlen(spec->title));
    interop_put_uint(&writer, InteropTagState, 0);
    interop_put_uint(&writer, InteropTagDpi, 96);
    if (spec->has_min_size) {
        uint8_t minimum[8];
        for (unsigned byte = 0; byte < 4; ++byte) {
            minimum[byte] = (uint8_t)(spec->min_width >> (8 * byte));
            minimum[4 + byte] = (uint8_t)(spec->min_height >> (8 * byte));
        }
        interop_put_bytes(&writer, InteropTagMinSize, minimum, sizeof minimum);
    }
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowUpdate, 0, InteropFromGuest, &length) != InteropErrorNone) return 0;
    return length;
}

static size_t build_retire(uint8_t *frame, size_t capacity, uint64_t window, uint64_t generation) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowRetire, 0, InteropFromGuest, &length) != InteropErrorNone) return 0;
    return length;
}

static host_wayland_status_t update_frame(const update_spec_t *spec) {
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_update(frame, sizeof frame, spec);
    if (length == 0) return HostWaylandMalformedFrame;
    return host_wayland_windows_handle_frame(&session_ctx, frame, length, 1000);
}

static host_wayland_status_t retire_frame(uint64_t window, uint64_t generation) {
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_retire(frame, sizeof frame, window, generation);
    if (length == 0) return HostWaylandMalformedFrame;
    return host_wayland_windows_handle_frame(&session_ctx, frame, length, 1000);
}

/** @brief Borrowed fixture proxies (compositor, wm_base, decoration manager) that the module never destroys. */
static bool is_fixture_proxy(unsigned index) {
    const char *iface = fake_proxies[index].iface;
    return iface && (!strcmp(iface, "wl_compositor") || !strcmp(iface, "xdg_wm_base") || !strcmp(iface, "zxdg_decoration_manager_v1") || !strcmp(iface, "wl_seat"));
}

/** @brief Index of the destroy request sent on `proxy`, or -1. */
static int destroy_index_of(unsigned proxy) {
    for (unsigned index = 0; index < fake_request_count; ++index) {
        if (fake_requests[index].destroy && fake_requests[index].proxy == proxy) return (int)index;
    }
    return -1;
}

/** @brief Toplevel 1 with a dialog 2, a menu 3 under it, a tooltip 4 under the menu, and an unrelated toplevel 5. */
static int build_tree(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t dialog = toplevel_spec(2, 1, "a-child");
    dialog.role = 2;
    dialog.has_parent = true;
    dialog.parent = 1;
    dialog.parent_generation = 1;
    Check(announce_spec(&session_ctx, &dialog) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 40, 40, 300, 200);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    announce_spec_t tooltip = menu_spec(4, 3, 1, 4, 40, 150, 40, 40);
    Check(announce_spec(&session_ctx, &tooltip) == HostWaylandOk);
    announce_spec_t other = toplevel_spec(5, 1, "a-other");
    Check(announce_spec(&session_ctx, &other) == HostWaylandOk);
    return 0;
}

static int check_update_title_and_size(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-root");
    spec.title = "One";
    Check(announce_spec(&session_ctx, &spec) == HostWaylandOk);
    unsigned titles = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE);
    update_spec_t same = {1, 1, false, 0, 0, "One", false, 0, 0};
    Check(update_frame(&same) == HostWaylandOk);
    /* A title equal to the stored one is not sent again. */
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE) == titles);
    update_spec_t changed = {1, 1, false, 0, 0, "Two", false, 0, 0};
    Check(update_frame(&changed) == HostWaylandOk);
    int title = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE, fake_request_count - 1);
    Check(title >= 0 && !strcmp(fake_requests[title].text, "Two"));
    update_spec_t minimum = {1, 1, false, 0, 0, NULL, true, 1200, 602};
    Check(update_frame(&minimum) == HostWaylandOk);
    int minimum_request = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_MIN_SIZE, 0);
    Check(minimum_request >= 0 && fake_requests[minimum_request].a0 == 1200 && fake_requests[minimum_request].a1 == 602);
    /* An update for a window that was never realized is stale and refused. */
    update_spec_t stale = {9, 1, false, 0, 0, "x", false, 0, 0};
    Check(update_frame(&stale) == HostWaylandRefused);
    Check(captured.last_refused_type == 0x0101 && captured.last_error_code == InteropErrorStaleWindow);
    return 0;
}

static int check_update_parent(void) {
    Check(build_tree() == 0);
    /* A Dialog moves to another realized toplevel: the table accepts it and set_parent follows. */
    announce_spec_t other_root = toplevel_spec(6, 1, "a-second");
    Check(announce_spec(&session_ctx, &other_root) == HostWaylandOk);
    unsigned before = fake_request_count;
    update_spec_t move = {2, 1, true, 6, 1, NULL, false, 0, 0};
    Check(update_frame(&move) == HostWaylandOk);
    int parent = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_PARENT, before);
    unsigned second = fake_nth("xdg_toplevel", 3);
    Check(parent >= 0 && fake_requests[parent].proxy == fake_nth("xdg_toplevel", 1) && fake_requests[parent].a0 == (long long)second);
    host_window_entry_t entry;
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){2, 1}, &entry) && entry.parent.window == 6);

    /* A parent that is not realized is refused, and the dialog keeps its parent. */
    update_spec_t missing = {2, 1, true, 9, 1, NULL, false, 0, 0};
    Check(update_frame(&missing) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorStaleWindow);
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){2, 1}, &entry) && entry.parent.window == 6);

    /* A toplevel never changes parent; a popup keeps the parent it was realized with. */
    update_spec_t toplevel = {5, 1, true, 1, 1, NULL, false, 0, 0};
    Check(update_frame(&toplevel) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorProtocolViolation);
    update_spec_t popup = {4, 1, true, 1, 1, NULL, false, 0, 0};
    Check(update_frame(&popup) == HostWaylandRefused);
    Check(captured.last_error_code == InteropErrorProtocolViolation);
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){4, 1}, &entry) && entry.parent.window == 3);
    return 0;
}

static int check_retire_order(void) {
    Check(build_tree() == 0);
    unsigned root_toplevel = fake_nth("xdg_toplevel", 0);
    unsigned dialog_toplevel = fake_nth("xdg_toplevel", 1);
    unsigned menu = fake_nth("xdg_popup", 0);
    unsigned tooltip = fake_nth("xdg_popup", 1);
    unsigned other_toplevel = fake_nth("xdg_toplevel", 2);
    unsigned root_decoration = fake_nth("zxdg_toplevel_decoration_v1", 0);
    unsigned root_surface = fake_nth("xdg_surface", 0);
    unsigned root_wl_surface = fake_nth("wl_surface", 0);
    Check(retire_frame(1, 1) == HostWaylandOk);

    /* Every window of the subtree is destroyed child first: tooltip, menu, dialog, root. */
    int tooltip_destroy = destroy_index_of(tooltip), menu_destroy = destroy_index_of(menu);
    int dialog_destroy = destroy_index_of(dialog_toplevel), root_destroy = destroy_index_of(root_toplevel);
    Check(tooltip_destroy >= 0 && menu_destroy > tooltip_destroy);
    Check(dialog_destroy >= 0 && root_destroy > dialog_destroy && root_destroy > menu_destroy);
    /* Within a window: decoration before its toplevel or popup, then xdg_surface, then wl_surface. */
    int decoration_destroy = destroy_index_of(root_decoration);
    Check(decoration_destroy >= 0 && decoration_destroy < root_destroy);
    Check(destroy_index_of(root_surface) > root_destroy);
    Check(destroy_index_of(root_wl_surface) > destroy_index_of(root_surface));
    /* The unrelated toplevel and its objects are untouched. */
    Check(destroy_index_of(other_toplevel) < 0);
    Check(host_wayland_windows_window_count(&session_ctx) == 1);
    Check(host_wayland_windows_is_mapped(&session_ctx, 5, 1) == false);
    return 0;
}

static int check_retire_stale_and_invalid(void) {
    Check(build_tree() == 0);
    unsigned before = fake_request_count;
    Check(retire_frame(9, 1) == HostWaylandOk);
    Check(retire_frame(1, 2) == HostWaylandOk);
    Check(fake_request_count == before);
    Check(retire_frame(0, 1) == HostWaylandMalformedFrame);
    Check(host_wayland_windows_window_count(&session_ctx) == 5);
    return 0;
}

static int check_late_events_after_retire(void) {
    Check(build_tree() == 0);
    unsigned root_toplevel = fake_nth("xdg_toplevel", 0);
    unsigned root_surface = fake_nth("xdg_surface", 0);
    unsigned menu = fake_nth("xdg_popup", 0);
    Check(retire_frame(1, 1) == HostWaylandOk);
    unsigned acks = fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE);
    deliver_surface_configure(root_surface, 3);
    deliver_toplevel_configure(root_toplevel, 100, 100, NULL, 0);
    deliver_toplevel_close(root_toplevel);
    deliver_popup_configure(menu, 1, 2, 3, 4);
    deliver_popup_done(menu);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == acks);
    Check(captured.configures == 0 && captured.closes == 0 && captured.repositioned == 0 && captured.dismissed == 0);
    return 0;
}

static int check_retire_releases_press(void) {
    Check(build_tree() == 0);
    Check(host_wayland_windows_record_press(&session_ctx, 300, 40, (struct wl_seat *)fake_object(fake_new("wl_seat")), 2, 1, 1000) == HostWaylandOk);
    Check(press_find(&session_ctx, 300, 1000) != NULL);
    Check(retire_frame(2, 1) == HostWaylandOk);
    Check(press_find(&session_ctx, 300, 1000) == NULL);
    return 0;
}

static int check_reset(void) {
    Check(build_tree() == 0);
    unsigned creates = fake_creates, destroys = fake_destroys;
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandOk);
    Check(!session_ctx.initialized && host_wayland_windows_window_count(&session_ctx) == 0);
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandNotInitialized);
    /* Reset destroys exactly the objects that were alive: created minus destroyed minus the borrowed fixtures. */
    Check(fake_destroys - destroys == creates - destroys - fixture_creates);
    Check(fake_creates - fixture_creates == fake_destroys && fake_double_destroys == 0);
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (is_fixture_proxy(index)) continue;
        Check(!fake_proxies[index].live);
    }
    /* Teardown is children first across the whole tree. */
    Check(destroy_index_of(fake_nth("xdg_popup", 1)) < destroy_index_of(fake_nth("xdg_popup", 0)));
    Check(destroy_index_of(fake_nth("xdg_popup", 0)) < destroy_index_of(fake_nth("xdg_toplevel", 0)));
    Check(destroy_index_of(fake_nth("xdg_toplevel", 1)) < destroy_index_of(fake_nth("xdg_toplevel", 0)));

    /* A corrupt table cannot give an order, but every record still goes: none may leak. */
    Check(build_tree() == 0);
    session_ctx.map.realized_count = 99;
    creates = fake_creates;
    destroys = fake_destroys;
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandOk);
    Check((fake_destroys - destroys) > 0 && fake_double_destroys == 0);
    Check(fake_creates - fixture_creates == fake_destroys);
    return 0;
}

#include "host_wayland_windows_fixtures.h"
#include "host_wayland_windows_stress.h"
#include "host_wayland_windows_defensive.h"

int main(void) {
    if (check_init() != 0) return 1;
    if (check_scale() != 0) return 1;
    if (check_presses() != 0) return 1;
    if (check_toplevel_realization() != 0) return 1;
    if (check_uac_and_dialog() != 0) return 1;
    if (check_min_size() != 0) return 1;
    if (check_configure_and_close() != 0) return 1;
    if (check_refusals() != 0) return 1;
    if (check_rollback_and_stale() != 0) return 1;
    if (check_malformed_and_lifecycle() != 0) return 1;
    if (check_decoration_request() != 0) return 1;
    if (check_decoration_staging() != 0) return 1;
    if (check_server_side_fallback() != 0) return 1;
    if (check_invalid_mode() != 0) return 1;
    if (check_decoration_failures() != 0) return 1;
    if (check_popup_realization() != 0) return 1;
    if (check_popup_positioner_versions() != 0) return 1;
    if (check_popup_grab() != 0) return 1;
    if (check_nested_popup() != 0) return 1;
    if (check_popup_reposition_and_dismiss() != 0) return 1;
    if (check_popup_refusals_and_rollback() != 0) return 1;
    if (check_update_title_and_size() != 0) return 1;
    if (check_update_parent() != 0) return 1;
    if (check_retire_order() != 0) return 1;
    if (check_retire_stale_and_invalid() != 0) return 1;
    if (check_late_events_after_retire() != 0) return 1;
    if (check_retire_releases_press() != 0) return 1;
    if (check_reset() != 0) return 1;
    if (check_codec_fixtures() != 0) return 1;
    if (check_stress() != 0) return 1;
    if (check_decoder_refusals() != 0) return 1;
    if (check_frame_refusals() != 0) return 1;
    if (check_binding_edges() != 0) return 1;
    if (check_null_callbacks() != 0) return 1;
    if (check_reposition_overflow() != 0) return 1;
    if (check_dismiss_desync() != 0) return 1;
    if (check_zero_parent_size() != 0) return 1;
    if (check_mirror_to_popup_parent() != 0) return 1;
    if (check_root_walk_bound() != 0) return 1;
    if (check_missing_record() != 0) return 1;
    if (check_retire_and_reset_paths() != 0) return 1;
    puts("host_wayland_windows_test: all checks passed");
    return 0;
}
