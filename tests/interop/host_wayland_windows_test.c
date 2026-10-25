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

/* ---- callback capture -------------------------------------------------------------------------- */

typedef struct capture_t {
    unsigned configures, closes, repositioned, dismissed, errors;
    uint32_t last_bits, last_width, last_height, last_error_code;
    uint16_t last_refused_type;
    uint64_t last_window, last_generation;
} capture_t;

static capture_t captured;

static void cb_configure(void *user, uint64_t window, uint64_t generation, uint32_t bits, uint32_t width, uint32_t height) {
    (void)user;
    ++captured.configures;
    captured.last_window = window;
    captured.last_generation = generation;
    captured.last_bits = bits;
    captured.last_width = width;
    captured.last_height = height;
}

static void cb_close(void *user, uint64_t window, uint64_t generation) {
    (void)user;
    ++captured.closes;
    captured.last_window = window;
    captured.last_generation = generation;
}

static void cb_repositioned(void *user, uint64_t window, uint64_t generation, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    (void)user; (void)window; (void)generation; (void)x; (void)y; (void)width; (void)height;
    ++captured.repositioned;
}

static void cb_dismissed(void *user, uint64_t window, uint64_t generation) {
    (void)user; (void)window; (void)generation;
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

/** @brief Fresh fake session with a live context; the context lives in static storage. */
static host_wayland_t session_ctx;

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
    /* Popups are realized by the popup layer, not here. */
    announce_spec_t menu = toplevel_spec(3, 1, "a-menu");
    menu.role = 3;
    Check(announce_spec(&session_ctx, &menu) == HostWaylandUnsupportedType);
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
    puts("host_wayland_windows_test: all checks passed");
    return 0;
}
