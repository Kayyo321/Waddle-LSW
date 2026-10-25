/**
 * @file session_windows_handler_test.c
 * @brief Tests of the session windows handler (src/interop/session_windows_handler.c).
 *
 * The test unit includes only the recording double (session_windows_handler_fake.h) and the public headers. The handler,
 * the Wayland core and the window realization are compiled as separate translation units and linked in; the double
 * replaces libwayland, the feature sender and nothing else. The window-state reducer is the C ABI object of
 * src/interop/window_state_abi.zig.
 *
 * Every test starts a fresh session (start_session) and ends it (end_session, or the runner's close) so that every
 * proxy and reducer is destroyed. The runner fails a test that leaves a protocol error in the double.
 */
#define _POSIX_C_SOURCE 200809L

#include "session_windows_handler_fake.h"

#include "session_windows_handler.h"
#include "waddle/interop_protocol.h"
#include "window_state.h"

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define Check(condition) do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

/* ---- session fixtures ----------------------------------------------------------------------------- */

static session_windows_handler_t handler;
static session_handler_t table;
static struct wl_display *display;
static uint64_t test_now;

static uint64_t test_clock(void) {
    return test_now;
}

/** @brief Globals the fixture announces. Each bit enables one global; the core requires compositor, wm_base and shm. */
enum {
    GlobalCompositor = 1u << 0,
    GlobalWmBase = 1u << 1,
    GlobalShm = 1u << 2,
    GlobalDecoration = 1u << 3,
    GlobalSeat = 1u << 4,
    GlobalAll = GlobalCompositor | GlobalWmBase | GlobalShm | GlobalDecoration | GlobalSeat
};

/**
 * @brief Start a session: fresh double, injected display, the globals in `globals`, and on_ready.
 * @return 0 when the handler is ready; 1 otherwise.
 */
static int start_session_with(uint32_t numerator, uint32_t denominator, uint32_t globals, uint64_t negotiated) {
    fake_begin();
    memset(&handler, 0, sizeof handler);
    test_now = 1000;
    display = (struct wl_display *)fake_object(fake_new("wl_display", 1));
    session_windows_config_t config;
    memset(&config, 0, sizeof config);
    config.device = "dev0";
    config.display = display;
    config.scale_numerator = numerator;
    config.scale_denominator = denominator;
    config.now_ms = test_clock;
    if (session_windows_handler_init(&handler, &config) != 0) return 1;
    table = session_windows_handler_descriptor(&handler);
    if (globals & GlobalCompositor) fake_queue_announce(1, "wl_compositor", 6);
    if (globals & GlobalWmBase) fake_queue_announce(2, "xdg_wm_base", 5);
    if (globals & GlobalShm) fake_queue_announce(3, "wl_shm", 1);
    if (globals & GlobalDecoration) fake_queue_announce(4, "zxdg_decoration_manager_v1", 1);
    if (globals & GlobalSeat) fake_queue_announce(5, "wl_seat", 7);
    int result = table.on_ready(table.context, negotiated, FakeSender);
    if (result != 0 || !handler.wayland_ready) return 1;
    return 0;
}

static int start_session(uint32_t numerator, uint32_t denominator) {
    return start_session_with(numerator, denominator, GlobalAll, InteropCapWindows);
}

/** @brief Close the session, then check that the double holds only the display and that no object was double-freed. */
static int end_session(void) {
    if (!handler.closing) table.on_closing(table.context, 0, FakeSender);
    Check(!handler.wayland_ready);
    Check(fake_live_count() == 1);
    Check(fake_live_pointer(display));
    Check(fake_creates - fake_destroys == 1);
    Check(fake_disconnects == 0);
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) Check(!handler.reducers[index].used);
    return 0;
}

/** @brief The double's protocol checks must all stay at zero (the file comment of the double lists them). */
static int check_double_clean(void) {
    Check(fake_protocol_errors == 0);
    Check(fake_dead_requests == 0);
    Check(fake_double_destroys == 0);
    Check(fake_order_violations == 0);
    Check(fake_invalid_serials == 0);
    Check(fake_invalid_grabs == 0);
    Check(fake_sender_mismatch == 0);
    return 0;
}

/** @brief Dispatch the queued compositor events the way on_fds does when the poll reports the display readable. */
static int pump(void) {
    struct pollfd fds[1];
    fds[0].fd = FakeFd;
    fds[0].events = POLLIN;
    fds[0].revents = POLLIN;
    return table.on_fds(table.context, fds, 1, test_now, FakeSender);
}

static int tick(void) {
    return table.on_tick(table.context, test_now, FakeSender);
}

static int send_frame(uint32_t type, const uint8_t *frame, size_t length) {
    return table.on_frame(table.context, type, 0, frame, length, FakeSender);
}

static session_windows_counters_t counters_now(void) {
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    return counters;
}

static struct wl_seat *session_seat(void) {
    return host_wayland_core_seat_at(&handler.core, 0);
}

static host_wayland_record_t *record_of(uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *rec = &handler.windows.records[index];
        if (rec->generation != 0 && rec->window == window && rec->generation == generation) return rec;
    }
    return NULL;
}

/** @brief Index of the double's proxy an object pointer names; FakeNone when the record has no such object. */
static unsigned index_of(const void *object) {
    return object ? fake_index(object) : FakeNone;
}

/* ---- guest frame builders (real codec writer) ----------------------------------------------------- */

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
    spec.role = InteropRoleToplevel;
    spec.x = 10;
    spec.y = 20;
    spec.width = 640;
    spec.height = 480;
    spec.app_key = app_key;
    spec.dpi = 96;
    return spec;
}

static announce_spec_t child_spec(uint64_t window, uint64_t generation, uint32_t role, uint64_t parent, uint64_t parent_generation, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    announce_spec_t spec = toplevel_spec(window, generation, "a-child");
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

/** @brief Announce a frame built from `spec` through on_frame. Returns the on_frame result. */
static int send_announce(const announce_spec_t *spec) {
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, spec);
    if (length == 0) return 1;
    return send_frame(InteropMsgWindowAnnounce, frame, length);
}

/** @brief Announce a toplevel-like window and stage its decoration mode so its first configure is accepted (2.6). */
static int announce_mapped(const announce_spec_t *spec) {
    Check(send_announce(spec) == 0);
    host_wayland_record_t *rec = record_of(spec->window, spec->generation);
    if (rec && rec->decoration) fake_queue_decoration_configure(index_of(rec->decoration), ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    Check(pump() == 0);
    return 0;
}

typedef struct update_spec_t {
    uint64_t window;
    uint64_t generation;
    int32_t x, y;
    uint32_t width, height;
    bool has_parent;
    uint64_t parent;
    uint64_t parent_generation;
    const char *title; /* NULL omits the title */
    bool has_min_size;
    uint32_t min_width, min_height;
    uint32_t state;
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
    interop_put_rect(&writer, InteropTagRect, spec->x, spec->y, spec->width, spec->height);
    if (spec->title) interop_put_bytes(&writer, InteropTagTitle, spec->title, (uint16_t)strlen(spec->title));
    interop_put_uint(&writer, InteropTagState, spec->state);
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

static int send_update(const update_spec_t *spec) {
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_update(frame, sizeof frame, spec);
    if (length == 0) return 1;
    return send_frame(InteropMsgWindowUpdate, frame, length);
}

static int send_retire(uint64_t window, uint64_t generation) {
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowRetire, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return send_frame(InteropMsgWindowRetire, frame, length);
}

/**
 * @brief Build a guest frame of one of the request types. The codec writer requires the fields in ascending tag order
 * (Edge 0x0F before Serial 0x10), so the optional fields are written in that order.
 */
static int send_request_frame(uint32_t type, uint64_t window, uint64_t generation, int has_serial, uint32_t serial, int has_edge, uint32_t edge, int has_state, uint32_t state) {
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    if (has_edge) interop_put_uint(&writer, InteropTagEdge, edge);
    if (has_serial) interop_put_uint(&writer, InteropTagSerial, serial);
    if (has_state) interop_put_uint(&writer, InteropTagState, state);
    size_t length = 0;
    if (interop_writer_finish(&writer, (uint16_t)type, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return send_frame(type, frame, length);
}

static int send_move(uint64_t window, uint64_t generation, uint32_t serial) {
    return send_request_frame(InteropMsgWindowMoveRequest, window, generation, 1, serial, 0, 0, 0, 0);
}

/**
 * @brief Resize request whose Edge byte is `raw_edge` as the handler receives it. The codec writer refuses edges outside
 * the valid set, so the frame is encoded with a valid edge and its Edge value is then overwritten in place.
 */
static int send_resize_raw_edge(uint64_t window, uint64_t generation, uint32_t serial, uint8_t raw_edge) {
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagEdge, InteropEdgeBottomRight);
    interop_put_uint(&writer, InteropTagSerial, serial);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowResizeRequest, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    const uint8_t *edge = NULL;
    uint16_t edge_length = 0;
    if (interop_field_get(frame + InteropHeaderBytes, length - InteropHeaderBytes, InteropTagEdge, 0u, &edge, &edge_length) != 0) return 1;
    ((uint8_t *)(void *)edge)[0] = raw_edge;
    return send_frame(InteropMsgWindowResizeRequest, frame, length);
}

static int send_resize(uint64_t window, uint64_t generation, uint32_t serial, uint32_t edge) {
    return send_resize_raw_edge(window, generation, serial, (uint8_t)edge);
}

static int send_state_request(uint64_t window, uint64_t generation, uint32_t state) {
    return send_request_frame(InteropMsgWindowStateRequest, window, generation, 0, 0, 0, 0, 1, state);
}

static int send_frame_ready(uint64_t window, uint64_t generation, uint32_t slot, uint64_t sequence) {
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagSlot, slot);
    interop_put_uint(&writer, InteropTagFrameSequence, sequence);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgFrameReady, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return send_frame(InteropMsgFrameReady, frame, length);
}

/**
 * @brief A frame of the window and generation fields only, delivered under a type that requires more. The writer
 * refuses the short payload for its own type, so the short frame is built as a Retire and sent as the other type: the
 * handler must count the missing field and change nothing.
 */
static int send_short_frame_as(uint32_t type, uint64_t window, uint64_t generation) {
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowRetire, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return send_frame(type, frame, length);
}

static int send_icon_changed(uint64_t window, uint64_t generation) {
    uint8_t frame[InteropMaxFrameBytes];
    uint8_t hash[32];
    memset(hash, 0x5A, sizeof hash);
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof frame);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_bytes(&writer, InteropTagContentHash, hash, sizeof hash);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowIconChanged, 0, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return send_frame(InteropMsgWindowIconChanged, frame, length);
}

/* ---- presses ----------------------------------------------------------------------------------------- */

static host_wayland_status_t press(uint32_t serial, uint32_t wayland_serial, struct wl_seat *seat, uint64_t window, uint64_t generation, uint32_t button, uint64_t lease, uint64_t now) {
    host_wayland_press_input_t input;
    memset(&input, 0, sizeof input);
    input.protocol_serial = serial;
    input.wayland_serial = wayland_serial;
    input.seat = seat;
    input.window = window;
    input.generation = generation;
    input.lease_epoch = lease;
    input.kind = HostWaylandPressPointer;
    input.button = button;
    input.now_ms = now;
    return session_windows_record_press(&handler, &input);
}

/* ---- outbound frames ------------------------------------------------------------------------------- */

typedef struct outbound_t {
    uint32_t type;
    uint32_t request_id;
    uint64_t window;
    uint64_t generation;
    int has_rect;
    int32_t rect[4];
    int has_state;
    uint32_t state;
    int has_slot;
    uint32_t slot;
    int has_sequence;
    uint64_t sequence;
} outbound_t;

/** @brief Decode one outbound frame and validate it as host-to-guest. Returns 0 on success. */
static int decode_outbound(const uint8_t *frame, size_t length, outbound_t *out) {
    memset(out, 0, sizeof *out);
    interop_header_t header;
    if (interop_header_decode(frame, length, &header) != InteropErrorNone) return 1;
    if (header.length != length - InteropHeaderBytes) return 1;
    const uint8_t *payload = frame + InteropHeaderBytes;
    if (interop_payload_validate(header.type, InteropFromHost, payload, header.length) != InteropErrorNone) return 1;
    out->type = header.type;
    out->request_id = header.request_id;
    if (interop_field_uint(payload, header.length, InteropTagWindow, 8u, &out->window) != 0) return 1;
    if (interop_field_uint(payload, header.length, InteropTagGeneration, 8u, &out->generation) != 0) return 1;
    const uint8_t *rect = NULL;
    uint16_t rect_length = 0;
    int found = interop_field_get(payload, header.length, InteropTagRect, 0u, &rect, &rect_length);
    if (found < 0 || (found == 0 && rect_length != 16u)) return 1;
    if (found == 0) {
        out->has_rect = 1;
        for (unsigned field = 0; field < 4; ++field) {
            out->rect[field] = (int32_t)((uint32_t)rect[4 * field] | ((uint32_t)rect[4 * field + 1] << 8) | ((uint32_t)rect[4 * field + 2] << 16) | ((uint32_t)rect[4 * field + 3] << 24));
        }
    }
    uint64_t wide = 0;
    if (interop_field_uint(payload, header.length, InteropTagState, 4u, &wide) == 0) {
        out->has_state = 1;
        out->state = (uint32_t)wide;
    }
    if (interop_field_uint(payload, header.length, InteropTagSlot, 4u, &wide) == 0) {
        out->has_slot = 1;
        out->slot = (uint32_t)wide;
    }
    if (interop_field_uint(payload, header.length, InteropTagFrameSequence, 8u, &wide) == 0) {
        out->has_sequence = 1;
        out->sequence = wide;
    }
    return 0;
}

static int outbound_at(unsigned index, outbound_t *out) {
    Check(index < fake_frame_count);
    Check(decode_outbound(fake_frames[index], fake_frame_length[index], out) == 0);
    return 0;
}

/** @brief Signature of the requests recorded from `from` on, compared with `expected`. */
static int expect_signature(unsigned from, const char *expected) {
    char actual[4096];
    fake_signature(actual, sizeof actual, from);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "signature mismatch\n  expected: %s\n  actual:   %s\n", expected, actual);
        return 1;
    }
    return 0;
}

/** @brief Text argument of the first request of `iface`/`opcode` at or after `from`. */
static int request_text(const char *iface, uint32_t opcode, unsigned from, const char **text) {
    int index = fake_find(iface, opcode, from);
    Check(index >= 0);
    *text = fake_requests[index].text;
    return 0;
}


/* ---- test 1: announce requests -------------------------------------------------------------------- */

static int test_announce_requests(void) {
    Check(start_session(1, 1) == 0);
    fake_clear_recording();

    /* Toplevel with a title and decoration. Request order: surface, xdg_surface, toplevel, app id, title, decoration,
     * mode, commit. The reducer then plans its initial observation: unset maximized, unset fullscreen (state 0). */
    announce_spec_t toplevel = toplevel_spec(1, 1, "a-app");
    toplevel.title = "Title";
    unsigned mark = fake_request_count;
    Check(send_announce(&toplevel) == 0);
    Check(expect_signature(mark,
        "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 xdg_toplevel:2 "
        "zxdg_decoration_manager_v1:1 zxdg_toplevel_decoration_v1:1 wl_surface:6 xdg_toplevel:10 xdg_toplevel:12") == 0);
    host_wayland_record_t *root = record_of(1, 1);
    Check(root != NULL && root->toplevel != NULL && root->decoration != NULL);
    const char *text = NULL;
    Check(request_text("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, mark, &text) == 0);
    Check(strcmp(text, "waddle.dev0.a-app") == 0);
    Check(request_text("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE, mark, &text) == 0);
    Check(strcmp(text, "Title") == 0);
    int get_decoration = fake_find("zxdg_decoration_manager_v1", ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION, mark);
    Check(get_decoration >= 0 && fake_requests[get_decoration].a0 == (long long)index_of(root->toplevel));
    int set_mode = fake_find("zxdg_toplevel_decoration_v1", ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE, mark);
    Check(set_mode >= 0 && fake_requests[set_mode].a0 == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    Check(fake_requests[fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, mark)].proxy == index_of(root->toplevel));
    Check(handler.counters.announces_realized == 1 && handler.counters.guest_observations_applied == 1);

    /* Dialog under the toplevel: set_parent names the parent's toplevel before the minimum size, and the app id is the
     * dialog's own key. No title, so no set_title. */
    mark = fake_request_count;
    announce_spec_t dialog = child_spec(2, 1, InteropRoleDialog, 1, 1, 0, 0, 300, 200);
    dialog.title = NULL;
    dialog.has_min_size = true;
    dialog.min_width = 100;
    dialog.min_height = 50;
    dialog.app_key = "a-dialog";
    Check(send_announce(&dialog) == 0);
    Check(expect_signature(mark,
        "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 xdg_toplevel:1 xdg_toplevel:8 "
        "zxdg_decoration_manager_v1:1 zxdg_toplevel_decoration_v1:1 wl_surface:6 xdg_toplevel:10 xdg_toplevel:12") == 0);
    host_wayland_record_t *child = record_of(2, 1);
    Check(child != NULL);
    int set_parent = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_PARENT, mark);
    Check(set_parent >= 0 && fake_requests[set_parent].a0 == (long long)index_of(root->toplevel));
    int min_size = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_MIN_SIZE, mark);
    Check(min_size >= 0 && fake_requests[min_size].a0 == 100 && fake_requests[min_size].a1 == 50);
    Check(request_text("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, mark, &text) == 0);
    Check(strcmp(text, "waddle.dev0.a-dialog") == 0);

    /* UAC consent dialog: its app id is fixed to the uac key, whatever the announced key. */
    mark = fake_request_count;
    announce_spec_t consent = toplevel_spec(3, 1, "a-uac");
    consent.role = InteropRoleUacConsent;
    Check(send_announce(&consent) == 0);
    Check(request_text("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, mark, &text) == 0);
    Check(strcmp(text, "waddle.dev0.uac") == 0);

    /* Menu popup under the toplevel, no press: positioner first, the popup, the positioner destroy, then the commit.
     * Its initial observation is refused for a popup, which is counted (see the popup test below). */
    mark = fake_request_count;
    announce_spec_t menu = child_spec(4, 1, InteropRoleMenu, 1, 1, 30, 12, 40, 20);
    Check(send_announce(&menu) == 0);
    Check(expect_signature(mark,
        "wl_compositor:0 xdg_wm_base:2 xdg_wm_base:1 xdg_positioner:1 xdg_positioner:2 xdg_positioner:3 xdg_positioner:4 "
        "xdg_positioner:6 xdg_positioner:5 xdg_positioner:7 xdg_surface:2 xdg_positioner:0! wl_surface:6") == 0);
    host_wayland_record_t *popup = record_of(4, 1);
    Check(popup != NULL && popup->popup != NULL);
    int get_popup = fake_find("xdg_surface", XDG_SURFACE_GET_POPUP, mark);
    Check(get_popup >= 0 && fake_requests[get_popup].a0 == (long long)index_of(root->xdg_surface));
    Check(fake_requests[get_popup].a1 >= 0);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}


/* ---- test 2: configure, close and popup events ----------------------------------------------------- */

static int test_event_frames(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    host_wayland_record_t *rec = record_of(1, 1);
    Check(rec != NULL);
    unsigned toplevel = index_of(rec->toplevel);
    unsigned surface = index_of(rec->xdg_surface);
    fake_clear_recording();

    /* Maximized and activated, 800 by 600 at scale 1: state bits Maximized|Activated, rect 0 0 800 600. */
    uint32_t states[] = {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED};
    fake_queue_toplevel_configure(toplevel, 800, 600, states, 2);
    fake_queue_surface_configure(surface, 7);
    Check(pump() == 0);
    Check(fake_frame_count == 1);
    outbound_t out;
    Check(outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgWindowConfigure && out.request_id == 0);
    Check(out.window == 1 && out.generation == 1);
    Check(out.has_rect && out.rect[0] == 0 && out.rect[1] == 0 && out.rect[2] == 800 && out.rect[3] == 600);
    Check(out.has_state && out.state == (WaddleWindowStateMaximized | WaddleWindowStateActivated));

    /* A zero extent is "the client decides": no rect, and the state is the Activated bit alone. */
    fake_clear_recording();
    uint32_t activated[] = {XDG_TOPLEVEL_STATE_ACTIVATED};
    fake_queue_toplevel_configure(toplevel, 0, 0, activated, 1);
    fake_queue_surface_configure(surface, 8);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgWindowConfigure && !out.has_rect && out.has_state && out.state == WaddleWindowStateActivated);

    /* A configure of one zero extent with the other nonzero is also "the client decides". */
    fake_clear_recording();
    fake_queue_toplevel_configure(toplevel, 640, 0, NULL, 0);
    fake_queue_surface_configure(surface, 9);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(!out.has_rect && out.has_state && out.state == 0);
    Check(handler.counters.configures_sent == 3);

    /* Close: WindowClose names the pair, with no rect and no state. */
    fake_clear_recording();
    fake_queue_toplevel_close(toplevel);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgWindowClose && out.request_id == 0 && out.window == 1 && out.generation == 1);
    Check(!out.has_rect && !out.has_state);
    Check(handler.counters.closes_sent == 1);

    /* Popup: a menu under the toplevel at guest (30, 12, 40, 20). The compositor places it at (15, 6); the repositioned
     * frame carries the guest coordinates of that placement, at scale 1 the same numbers. */
    announce_spec_t menu = child_spec(4, 1, InteropRoleMenu, 1, 1, 30, 12, 40, 20);
    Check(send_announce(&menu) == 0);
    host_wayland_record_t *popup_rec = record_of(4, 1);
    Check(popup_rec != NULL && popup_rec->popup != NULL);
    unsigned popup = index_of(popup_rec->popup);
    fake_clear_recording();
    fake_queue_popup_configure(popup, 15, 6, 40, 20);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgPopupRepositioned && out.request_id == 0 && out.window == 4 && out.generation == 1);
    Check(out.has_rect && out.rect[0] == 15 && out.rect[1] == 6 && out.rect[2] == 40 && out.rect[3] == 20);

    /* popup_done: PopupDismissed names the pair with no rect. */
    fake_clear_recording();
    fake_queue_popup_done(popup);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgPopupDismissed && out.request_id == 0 && out.window == 4 && out.generation == 1 && !out.has_rect);
    Check(handler.counters.popup_frames_sent == 2);

    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 2b: configure rects at scale 2 and the 16384 limit ------------------------------------- */

static int test_configure_scaled(void) {
    Check(start_session(2, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    host_wayland_record_t *rec = record_of(1, 1);
    unsigned toplevel = index_of(rec->toplevel);
    unsigned surface = index_of(rec->xdg_surface);
    outbound_t out;

    fake_clear_recording();
    fake_queue_toplevel_configure(toplevel, 320, 200, NULL, 0);
    fake_queue_surface_configure(surface, 1);
    Check(pump() == 0 && fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.has_rect && out.rect[2] == 640 && out.rect[3] == 400);

    /* 9000 logical by 10 is 18000 physical: over 16384, so no rect. */
    fake_clear_recording();
    fake_queue_toplevel_configure(toplevel, 9000, 10, NULL, 0);
    fake_queue_surface_configure(surface, 2);
    Check(pump() == 0 && fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(!out.has_rect);

    /* 8192 by 8 is exactly 16384 by 16: the limit itself is allowed. */
    fake_clear_recording();
    fake_queue_toplevel_configure(toplevel, 8192, 8, NULL, 0);
    fake_queue_surface_configure(surface, 3);
    Check(pump() == 0 && fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.has_rect && out.rect[2] == 16384 && out.rect[3] == 16);

    /* The reducer took the configured physical size: an observation with the same state needs no further action. */
    Check(handler.counters.configures_sent == 3);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 3: move and resize serial matrix -------------------------------------------------------- */

static unsigned moves_recorded(void) {
    return fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_MOVE);
}

static unsigned resizes_recorded(void) {
    return fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_RESIZE);
}

/**
 * @brief Send one move request and check its outcome: applied (one xdg move with the Wayland serial) or ignored (counted,
 * and no xdg request at all).
 */
static int move_case(uint64_t window, uint64_t generation, uint32_t serial, bool applied, uint32_t expected_wayland_serial) {
    session_windows_counters_t before = counters_now();
    unsigned moves = moves_recorded();
    unsigned mark = fake_request_count;
    Check(send_move(window, generation, serial) == 0);
    session_windows_counters_t after = counters_now();
    if (applied) {
        Check(after.moves_applied == before.moves_applied + 1);
        Check(after.moves_ignored == before.moves_ignored);
        Check(moves_recorded() == moves + 1);
        int move = fake_find("xdg_toplevel", XDG_TOPLEVEL_MOVE, mark);
        Check(move >= 0);
        Check(fake_requests[move].a0 == (long long)fake_index(session_seat()));
        Check(fake_requests[move].a1 == (long long)expected_wayland_serial);
    } else {
        Check(after.moves_applied == before.moves_applied);
        Check(after.moves_ignored == before.moves_ignored + 1);
        Check(moves_recorded() == moves);
    }
    return 0;
}

static int resize_case(uint64_t window, uint64_t generation, uint32_t serial, uint32_t edge, bool applied, uint32_t expected_wayland_serial) {
    session_windows_counters_t before = counters_now();
    unsigned resizes = resizes_recorded();
    unsigned mark = fake_request_count;
    Check(send_resize(window, generation, serial, edge) == 0);
    session_windows_counters_t after = counters_now();
    if (applied) {
        Check(after.resizes_applied == before.resizes_applied + 1);
        Check(after.resizes_ignored == before.resizes_ignored);
        Check(resizes_recorded() == resizes + 1);
        int resize = fake_find("xdg_toplevel", XDG_TOPLEVEL_RESIZE, mark);
        Check(resize >= 0);
        Check(fake_requests[resize].a0 == (long long)fake_index(session_seat()));
        Check(fake_requests[resize].a1 == (long long)expected_wayland_serial);
        Check(fake_requests[resize].a2 == (long long)edge);
    } else {
        Check(after.resizes_applied == before.resizes_applied);
        Check(after.resizes_ignored == before.resizes_ignored + 1);
        Check(resizes_recorded() == resizes);
    }
    return 0;
}

static int test_move_resize_matrix(void) {
    Check(start_session(1, 1) == 0);
    /* Window 1 is resizable and maximizable; window 2 has no capabilities. */
    announce_spec_t resizable = toplevel_spec(1, 1, "a-resize");
    resizable.state = WaddleWindowStateResizable | WaddleWindowStateMaximizable;
    Check(announce_mapped(&resizable) == 0);
    announce_spec_t plain = toplevel_spec(2, 1, "a-plain");
    Check(announce_mapped(&plain) == 0);
    struct wl_seat *seat = session_seat();
    Check(seat != NULL);
    host_wayland_record_t *one = record_of(1, 1);
    Check(one != NULL && one->toplevel != NULL);
    Check(handler.counters.announces_realized == 2);
    uint64_t now = test_now;

    /* Valid: a live left press of the current lease, latest for its window, with its own Wayland serial. */
    Check(press(11, 901, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(move_case(1, 1, 11, true, 901) == 0);

    /* Stale serial: never recorded. */
    Check(move_case(1, 1, 12, false, 0) == 0);

    /* Released press: recorded, then released, then named. */
    Check(press(13, 902, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    session_windows_release_press(&handler, 13);
    Check(move_case(1, 1, 13, false, 0) == 0);

    /* Wrong window generation: the press is for generation 1, the request names generation 2. */
    Check(press(14, 903, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(move_case(1, 2, 14, false, 0) == 0);

    /* Right window identity with a generation that is not realized: no reducer and no toplevel. */
    Check(press(15, 904, seat, 1, 2, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(move_case(1, 2, 15, false, 0) == 0);

    /* Not the left button, and not a pointer press: both ignored. */
    Check(press(16, 905, seat, 1, 1, 0x111u, 0, now) == HostWaylandOk);
    Check(move_case(1, 1, 16, false, 0) == 0);
    host_wayland_press_input_t touch;
    memset(&touch, 0, sizeof touch);
    touch.protocol_serial = 17;
    touch.wayland_serial = 906;
    touch.seat = seat;
    touch.window = 1;
    touch.generation = 1;
    touch.lease_epoch = 0;
    touch.kind = HostWaylandPressTouch;
    touch.now_ms = now;
    Check(session_windows_record_press(&handler, &touch) == HostWaylandOk);
    Check(move_case(1, 1, 17, false, 0) == 0);

    /* A seat the core never recorded: the press exists but its seat is not one the core still holds. */
    static struct wl_seat *const unknown_seat = (struct wl_seat *)(void *)&unknown_seat;
    Check(press(18, 907, unknown_seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(move_case(1, 1, 18, false, 0) == 0);

    /* Not the latest press of its window: the later press 20 is the one that counts. */
    Check(press(19, 908, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(press(20, 909, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    Check(move_case(1, 1, 19, false, 0) == 0);
    Check(move_case(1, 1, 20, true, 909) == 0);

    /* Expiry: a press is valid at 300 ms exactly and expired after it. */
    Check(press(21, 910, seat, 1, 1, SessionWindowsButtonLeft, 0, now) == HostWaylandOk);
    test_now = now + 301;
    Check(move_case(1, 1, 21, false, 0) == 0);
    test_now = now + 1000;
    Check(press(22, 911, seat, 1, 1, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    test_now += 300;
    Check(move_case(1, 1, 22, true, 911) == 0);

    /* Lease epoch mismatch: the press belongs to lease 2 while the handler holds lease 3. */
    session_windows_set_lease_epoch(&handler, 3);
    Check(press(23, 912, seat, 1, 1, SessionWindowsButtonLeft, 2, test_now) == HostWaylandOk);
    Check(move_case(1, 1, 23, false, 0) == 0);
    Check(press(24, 913, seat, 1, 1, SessionWindowsButtonLeft, 3, test_now) == HostWaylandOk);
    Check(move_case(1, 1, 24, true, 913) == 0);
    session_windows_set_lease_epoch(&handler, 0);

    /* Resize: valid edges of the resizable window; the reducer's Resizable capability is required. */
    Check(press(30, 920, seat, 1, 1, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    const uint32_t valid_edges[] = {1, 2, 4, 5, 6, 8, 9, 10};
    for (unsigned index = 0; index < sizeof valid_edges / sizeof valid_edges[0]; ++index) {
        Check(resize_case(1, 1, 30, valid_edges[index], true, 920) == 0);
    }
    /* Edges outside the set of 4.5 (0, 3, 7, 11, 12) are refused. */
    const uint32_t invalid_edges[] = {0, 3, 7, 11, 12};
    for (unsigned index = 0; index < sizeof invalid_edges / sizeof invalid_edges[0]; ++index) {
        Check(resize_case(1, 1, 30, invalid_edges[index], false, 0) == 0);
    }
    /* Resizable is missing on window 2, so a valid press and a valid edge are still refused. */
    Check(press(31, 921, seat, 2, 1, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    Check(resize_case(2, 1, 31, 10, false, 0) == 0);
    /* A resize naming a window that is not realized at all. */
    Check(press(32, 922, seat, 9, 9, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    Check(resize_case(9, 9, 32, 10, false, 0) == 0);

    /* Malformed move: no serial field, so the frame is counted and nothing else happens. */
    unsigned refused = handler.counters.frame_fields_refused;
    Check(send_short_frame_as(InteropMsgWindowMoveRequest, 1, 1) == 0);
    Check(counters_now().frame_fields_refused == refused + 1);

    Check(counters_now().moves_applied == 4);
    Check(counters_now().resizes_applied == 8);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* A seat removed by the compositor forgets the presses it owns, so a later request on them is refused. */
static int test_move_forgotten_seat(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    struct wl_seat *seat = session_seat();
    Check(seat != NULL);
    Check(press(40, 930, seat, 1, 1, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    fake_queue_remove(5);
    Check(pump() == 0);
    Check(host_wayland_core_seat_count(&handler.core) == 0);
    Check(move_case(1, 1, 40, false, 0) == 0);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 4: state requests ------------------------------------------------------------------------ */

static int test_state_requests(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t capable = toplevel_spec(1, 1, "a-max");
    capable.state = WaddleWindowStateMaximizable;
    Check(announce_mapped(&capable) == 0);
    announce_spec_t incapable = toplevel_spec(2, 1, "a-plain");
    Check(announce_mapped(&incapable) == 0);
    unsigned maximize = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED);
    unsigned unmaximize = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_UNSET_MAXIMIZED);
    session_windows_counters_t before = counters_now();

    /* Maximize with the capability: one set_maximized, the reducer's applied host state records it. */
    Check(send_state_request(1, 1, WaddleWindowStateMaximized) == 0);
    Check(counters_now().state_requests_applied == before.state_requests_applied + 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED) == maximize + 1);
    host_wayland_record_t *rec = record_of(1, 1);
    Check(rec != NULL && rec->toplevel != NULL);
    Check(handler.counters.host_actions_failed == 0);

    /* The same request again is handled and sends nothing: the state already holds it. */
    Check(send_state_request(1, 1, WaddleWindowStateMaximized) == 0);
    Check(counters_now().state_requests_applied == before.state_requests_applied + 2);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED) == maximize + 1);

    /* Clearing the bit unmaximizes. */
    Check(send_state_request(1, 1, 0) == 0);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_UNSET_MAXIMIZED) == unmaximize + 1);
    Check(counters_now().state_requests_applied == before.state_requests_applied + 3);

    /* Without Maximizable the maximize request is refused by the reducer and nothing is sent. */
    Check(send_state_request(2, 1, WaddleWindowStateMaximized) == 0);
    Check(counters_now().state_requests_ignored == before.state_requests_ignored + 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED) == maximize + 1);
    /* Unmaximize without the capability is allowed: nothing was maximized, so there is no request. */
    Check(send_state_request(2, 1, 0) == 0);
    Check(counters_now().state_requests_applied == before.state_requests_applied + 4);

    /* A window that is not realized: ignored at the lookup. */
    Check(send_state_request(9, 9, WaddleWindowStateMaximized) == 0);
    Check(counters_now().state_requests_ignored == before.state_requests_ignored + 2);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 5: FrameReady releases the slot at once ---------------------------------------------------- */

static int test_frame_ready(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    fake_clear_recording();

    Check(send_frame_ready(1, 1, 3, 42) == 0);
    Check(fake_frame_count == 1);
    uint8_t expected[InteropMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, expected, sizeof expected);
    interop_put_uint(&writer, InteropTagWindow, 1);
    interop_put_uint(&writer, InteropTagGeneration, 1);
    interop_put_uint(&writer, InteropTagSlot, 3);
    interop_put_uint(&writer, InteropTagFrameSequence, 42);
    size_t expected_length = 0;
    Check(interop_writer_finish(&writer, InteropMsgFrameRelease, 0, InteropFromHost, &expected_length) == InteropErrorNone);
    Check(fake_frame_length[0] == expected_length);
    Check(memcmp(fake_frames[0], expected, expected_length) == 0);
    outbound_t out;
    Check(outbound_at(0, &out) == 0);
    Check(out.type == InteropMsgFrameRelease && out.request_id == 0 && out.slot == 3 && out.sequence == 42);

    /* The frames layer is not built, so a pair that is not realized is released as well (the header limits). */
    fake_clear_recording();
    Check(send_frame_ready(5, 7, 0, 0xFFFFFFFFFFull) == 0);
    Check(fake_frame_count == 1 && outbound_at(0, &out) == 0);
    Check(out.window == 5 && out.generation == 7 && out.sequence == 0xFFFFFFFFFFull && out.slot == 0);
    Check(handler.counters.frames_dropped_no_buffer_layer == 2);

    /* A malformed FrameReady (no slot) is counted and sends nothing. */
    fake_clear_recording();
    Check(send_short_frame_as(InteropMsgFrameReady, 1, 1) == 0);
    Check(fake_frame_count == 0);
    Check(handler.counters.frame_fields_refused == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 6: send backpressure ----------------------------------------------------------------------- */

/** @brief Configure a toplevel and its surface, then pump; the feature sender decides what happens to the frame. */
static int configure_window(unsigned toplevel, unsigned surface, int32_t width, int32_t height, uint32_t serial) {
    fake_queue_toplevel_configure(toplevel, width, height, NULL, 0);
    fake_queue_surface_configure(surface, serial);
    return pump();
}

static int test_backpressure_coalesce(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    host_wayland_record_t *rec = record_of(1, 1);
    unsigned toplevel = index_of(rec->toplevel);
    unsigned surface = index_of(rec->xdg_surface);
    fake_clear_recording();

    /* The feature queue is full: the first configure is kept, the second for the same window replaces it. */
    fake_send_mode = FakeSendFull;
    Check(configure_window(toplevel, surface, 800, 600, 1) == 0);
    Check(handler.pending_count == 1 && fake_frame_count == 0);
    Check(fake_send_full >= 1);
    Check(configure_window(toplevel, surface, 800, 700, 2) == 0);
    Check(handler.pending_count == 1);
    Check(handler.counters.configures_coalesced == 1);
    /* A WindowClose is queued behind it and is not coalesced. */
    fake_queue_toplevel_close(toplevel);
    Check(pump() == 0);
    Check(handler.pending_count == 2);

    /* The queue drains in order on the next tick: the latest configure, then the close. */
    fake_send_mode = FakeSendAccept;
    Check(tick() == 0);
    Check(handler.pending_count == 0);
    Check(fake_frame_count == 2);
    outbound_t out;
    Check(outbound_at(0, &out) == 0 && out.type == InteropMsgWindowConfigure);
    Check(out.has_rect && out.rect[2] == 800 && out.rect[3] == 700);
    Check(outbound_at(1, &out) == 0 && out.type == InteropMsgWindowClose);
    Check(handler.counters.frames_queued == 2);
    Check(handler.counters.frames_sent == 2);
    Check(handler.counters.configures_sent == 2 && handler.counters.closes_sent == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

static int test_backpressure_closes_and_overflow(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t first = toplevel_spec(1, 1, "a-one");
    Check(announce_mapped(&first) == 0);
    announce_spec_t second = toplevel_spec(2, 1, "a-two");
    Check(announce_mapped(&second) == 0);
    unsigned one = index_of(record_of(1, 1)->toplevel);
    unsigned two = index_of(record_of(2, 1)->toplevel);
    fake_clear_recording();

    /* Three closes under backpressure are all kept, in order. */
    fake_send_mode = FakeSendFull;
    fake_queue_toplevel_close(one);
    fake_queue_toplevel_close(two);
    fake_queue_toplevel_close(one);
    Check(pump() == 0);
    Check(handler.pending_count == 3 && handler.counters.pending_overflow == 0);
    fake_send_mode = FakeSendAccept;
    Check(tick() == 0);
    Check(fake_frame_count == 3);
    Check(handler.counters.closes_sent == 3);

    /* 128 FrameRelease frames fill the list; the 129th fails the run and every later callback fails too. */
    fake_clear_recording();
    unsigned queued_before = handler.counters.frames_queued;
    fake_send_mode = FakeSendFull;
    for (unsigned index = 0; index < SessionWindowsMaxPending; ++index) {
        Check(send_frame_ready(1, 1, index % 4, index + 1) == 0);
    }
    Check(handler.pending_count == SessionWindowsMaxPending);
    Check(handler.counters.frames_queued == queued_before + SessionWindowsMaxPending);
    Check(send_frame_ready(1, 1, 0, 999) == 1);
    Check(handler.counters.pending_overflow == 1);
    Check(handler.pending_count == SessionWindowsMaxPending);
    Check(tick() == 1);
    Check(send_frame_ready(1, 1, 0, 1000) == 1);
    fake_send_mode = FakeSendAccept;
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

static int test_backpressure_draining_and_fail(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    unsigned toplevel = index_of(record_of(1, 1)->toplevel);
    unsigned surface = index_of(record_of(1, 1)->xdg_surface);
    fake_clear_recording();

    /* Draining: a frame refused with Draining is dropped and counted; the run continues. */
    fake_send_mode = FakeSendDraining;
    Check(send_frame_ready(1, 1, 0, 1) == 0);
    Check(handler.counters.frames_dropped_draining == 1 && handler.pending_count == 0);

    /* A queued list is dropped when the sender starts refusing with Draining at a flush. */
    fake_send_mode = FakeSendFull;
    Check(configure_window(toplevel, surface, 100, 100, 1) == 0);
    Check(handler.pending_count == 1);
    fake_send_mode = FakeSendDraining;
    Check(tick() == 0);
    Check(handler.pending_count == 0 && handler.counters.frames_dropped_draining == 2);

    /* Any other result fails the run at once, and the failure is sticky. */
    fake_send_mode = FakeSendFail;
    Check(send_frame_ready(1, 1, 0, 2) == 1);
    Check(handler.failed == 1);
    fake_send_mode = FakeSendAccept;
    Check(send_frame_ready(1, 1, 0, 3) == 1);
    Check(tick() == 1);
    Check(pump() == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 7: stale generations ------------------------------------------------------------------------ */

static int test_stale_generations(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-app");
    Check(announce_mapped(&root) == 0);
    unsigned old_toplevel = index_of(record_of(1, 1)->toplevel);
    unsigned old_surface = index_of(record_of(1, 1)->xdg_surface);

    /* Retire generation 1, then queue a close and a configure for its objects: the double drops events for dead
     * objects, as libwayland does, and nothing is sent. */
    Check(send_retire(1, 1) == 0);
    Check(!host_wayland_windows_contains(&handler.windows, 1, 1));
    fake_clear_recording();
    fake_queue_toplevel_close(old_toplevel);
    fake_queue_surface_configure(old_surface, 4);
    Check(pump() == 0);
    Check(fake_frame_count == 0);
    Check(handler.counters.closes_sent == 0 && handler.counters.configures_sent == 0);

    /* Generation 2 of the same window is realized. A move, a resize and a state request that name generation 1 are
     * refused; a frame for it is released without a check (the header limits). */
    announce_spec_t again = toplevel_spec(1, 2, "a-app");
    again.state = WaddleWindowStateResizable | WaddleWindowStateMaximizable;
    Check(announce_mapped(&again) == 0);
    struct wl_seat *seat = session_seat();
    Check(press(50, 940, seat, 1, 1, SessionWindowsButtonLeft, 0, test_now) == HostWaylandOk);
    session_windows_counters_t before = counters_now();
    Check(send_move(1, 1, 50) == 0);
    Check(send_resize(1, 1, 50, 10) == 0);
    Check(send_state_request(1, 1, WaddleWindowStateMaximized) == 0);
    session_windows_counters_t after = counters_now();
    Check(after.moves_ignored == before.moves_ignored + 1);
    Check(after.resizes_ignored == before.resizes_ignored + 1);
    Check(after.state_requests_ignored == before.state_requests_ignored + 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_MOVE) == 0);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_RESIZE) == 0);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED) == 0);
    Check(host_wayland_windows_contains(&handler.windows, 1, 2));

    /* The old generation's configure was dropped by the double, so the only frames are the ones for the new window. */
    Check(fake_frame_count == 0);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 8: display errors ----------------------------------------------------------------------- */

static int test_display_error_read(void) {
    Check(start_session(1, 1) == 0);
    fake_read_mode = FakeFault;
    Check(pump() == 1);
    Check(handler.failed == 1);
    /* Sticky: a later tick and a later frame both report the failure. */
    Check(tick() == 1);
    Check(send_frame_ready(1, 1, 0, 1) == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

static int test_display_error_dispatch(void) {
    Check(start_session(1, 1) == 0);
    fake_dispatch_fail_at = fake_dispatch_calls + 1;
    Check(pump() == 1);
    Check(tick() == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

static int test_display_flush_errors(void) {
    Check(start_session(1, 1) == 0);
    /* EAGAIN on flush is not an error: the handler waits for POLLOUT. */
    fake_flush_mode = FakeEagain;
    Check(tick() == 0);
    struct pollfd fds[1];
    size_t count = 0;
    Check(table.poll_fds(table.context, fds, 1, &count) == 0 && count == 1);
    Check((fds[0].events & POLLOUT) != 0);
    /* A flush that fails with a hard error is fatal and sticky. */
    fake_flush_mode = FakeEio;
    Check(tick() == 1);
    Check(tick() == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 9: teardown order ----------------------------------------------------------------------- */


/* ---- test 10: missing globals -------------------------------------------------------------------- */

static int test_missing_globals(void) {
    /* Without xdg_wm_base the core refuses the startup: on_ready fails and the connection is closed again. */
    fake_begin();
    memset(&handler, 0, sizeof handler);
    test_now = 1000;
    display = (struct wl_display *)fake_object(fake_new("wl_display", 1));
    session_windows_config_t config;
    memset(&config, 0, sizeof config);
    config.device = "dev0";
    config.display = display;
    config.now_ms = test_clock;
    Check(session_windows_handler_init(&handler, &config) == 0);
    table = session_windows_handler_descriptor(&handler);
    fake_queue_announce(1, "wl_compositor", 6);
    fake_queue_announce(3, "wl_shm", 1);
    fake_queue_announce(5, "wl_seat", 7);
    Check(table.on_ready(table.context, InteropCapWindows, FakeSender) == 1);
    Check(!handler.wayland_ready);
    table.on_closing(table.context, 0, FakeSender);
    Check(fake_live_count() == 1);
    Check(check_double_clean() == 0);

    /* Without the decoration manager the startup succeeds and no window gets a decoration object. */
    Check(start_session_with(1, 1, GlobalAll & ~GlobalDecoration, InteropCapWindows) == 0);
    unsigned mark = fake_request_count;
    announce_spec_t plain = toplevel_spec(1, 1, "a-plain");
    Check(send_announce(&plain) == 0);
    Check(fake_find("zxdg_decoration_manager_v1", ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION, mark) < 0);
    Check(record_of(1, 1)->decoration == NULL);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);

    /* A session that did not negotiate windows is not started, and the double sees no request at all. */
    Check(start_session_with(1, 1, GlobalAll, 0) == 1);
    Check(!handler.wayland_ready);
    Check(fake_creates == 1);
    table.on_closing(table.context, 0, FakeSender);
    Check(check_double_clean() == 0);

    /* A missing shm or compositor is refused the same way. */
    Check(start_session_with(1, 1, GlobalAll & ~GlobalShm, InteropCapWindows) == 1);
    table.on_closing(table.context, 0, FakeSender);
    Check(fake_live_count() == 1);
    Check(start_session_with(1, 1, GlobalAll & ~GlobalCompositor, InteropCapWindows) == 1);
    table.on_closing(table.context, 0, FakeSender);
    Check(fake_live_count() == 1);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 9: teardown order ----------------------------------------------------------------------- */

static int test_teardown_order(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_mapped(&root) == 0);
    announce_spec_t dialog = child_spec(2, 1, InteropRoleDialog, 1, 1, 0, 0, 300, 200);
    dialog.app_key = "a-dialog";
    Check(announce_mapped(&dialog) == 0);
    announce_spec_t menu = child_spec(3, 1, InteropRoleMenu, 1, 1, 30, 12, 40, 20);
    Check(send_announce(&menu) == 0);
    announce_spec_t nested = child_spec(4, 1, InteropRoleMenu, 3, 1, 5, 5, 10, 10);
    Check(send_announce(&nested) == 0);
    Check(host_wayland_windows_window_count(&handler.windows) == 4);
    fake_clear_recording();

    table.on_closing(table.context, 0, FakeSender);
    Check(handler.closing == 1 && !handler.wayland_ready);

    /* Windows first: a toplevel is decoration, toplevel, xdg_surface, wl_surface (4 objects); a popup is xdg_popup,
     * xdg_surface, wl_surface (3). Two toplevels and two popups: 14 objects. */
    unsigned window_objects = 0;
    unsigned decorations = 0, toplevels = 0, popups = 0, surfaces = 0, wl_surfaces = 0;
    for (unsigned index = 0; index < fake_log_count; ++index) {
        const char *iface = fake_log[index];
        if (!strcmp(iface, "zxdg_toplevel_decoration_v1")) ++decorations;
        else if (!strcmp(iface, "xdg_toplevel")) ++toplevels;
        else if (!strcmp(iface, "xdg_popup")) ++popups;
        else if (!strcmp(iface, "xdg_surface")) ++surfaces;
        else if (!strcmp(iface, "wl_surface")) ++wl_surfaces;
        else continue;
        ++window_objects;
    }
    Check(window_objects == 14);
    Check(decorations == 2 && toplevels == 2 && popups == 2 && surfaces == 4 && wl_surfaces == 4);
    Check(fake_order_violations == 0);

    /* Then the core, in its order: seat release, decoration manager, shm, xdg_wm_base, compositor, registry. The
     * display is borrowed: it is not destroyed and not disconnected. */
    Check(fake_log_count == 20);
    static const char *const core_order[] = {"wl_seat", "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry"};
    for (unsigned index = 0; index < 6; ++index) {
        Check(!strcmp(fake_log[14 + index], core_order[index]));
    }
    Check(fake_disconnects == 0);
    Check(fake_live_pointer(display));
    Check(fake_live_count() == 1);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- white-box reducer paths and the window lifecycle ---------------------------------------------- */

/** @brief Plan and complete a GuestObservation with success = 0: the reducer is left unsynchronized (a failed host step). */
static int desynchronize(uint64_t window, uint64_t generation) {
    int index = -1;
    for (uint32_t slot = 0; slot < SessionWindowsMaxReducers; ++slot) {
        if (handler.reducers[slot].used && handler.reducers[slot].window == window && handler.reducers[slot].generation == generation) index = (int)slot;
    }
    Check(index >= 0);
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof input);
    input.kind = WaddleWindowStateInputGuestObservation;
    input.state = 0;
    waddle_window_state_plan_t plan;
    Check(waddle_window_state_plan(handler.reducers[index].handle, window, (uint32_t)generation, &input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handler.reducers[index].handle, &plan, 0, 0) == WaddleWindowStateOk);
    return 0;
}

static int reducer_synced(uint64_t window, uint64_t generation, uint32_t *applied_host_state) {
    for (uint32_t slot = 0; slot < SessionWindowsMaxReducers; ++slot) {
        if (!handler.reducers[slot].used || handler.reducers[slot].window != window || handler.reducers[slot].generation != generation) continue;
        waddle_window_state_snapshot_t snapshot;
        Check(waddle_window_state_snapshot(handler.reducers[slot].handle, &snapshot) == WaddleWindowStateOk);
        if (applied_host_state) *applied_host_state = snapshot.applied_host_state;
        return (int)snapshot.synced;
    }
    return -1;
}

static int test_reducer_paths(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t capable = toplevel_spec(1, 1, "a-cap");
    capable.state = WaddleWindowStateResizable | WaddleWindowStateMaximizable;
    Check(announce_mapped(&capable) == 0);
    Check(reducer_synced(1, 1, NULL) == 1);
    unsigned toplevel = index_of(record_of(1, 1)->toplevel);
    unsigned surface = index_of(record_of(1, 1)->xdg_surface);

    /* An unsynchronized reducer: host requests are refused, configure and close still reach the guest. */
    Check(desynchronize(1, 1) == 0);
    Check(reducer_synced(1, 1, NULL) == 0);
    session_windows_counters_t before = counters_now();
    Check(send_state_request(1, 1, WaddleWindowStateMaximized) == 0);
    Check(counters_now().state_requests_ignored == before.state_requests_ignored + 1);
    fake_clear_recording();
    fake_queue_toplevel_configure(toplevel, 500, 400, NULL, 0);
    fake_queue_surface_configure(surface, 1);
    Check(pump() == 0);
    Check(fake_frame_count == 1);
    Check(counters_now().configures_sent == before.configures_sent + 1);
    Check(reducer_synced(1, 1, NULL) == 0);
    fake_clear_recording();
    fake_queue_toplevel_close(toplevel);
    Check(pump() == 0);
    Check(fake_frame_count == 1 && counters_now().closes_sent == before.closes_sent + 1);

    /* A guest observation resynchronizes: the reconcile plan unsets maximized and fullscreen, and the reducer is
     * synchronized again. */
    unsigned unmaximize = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_UNSET_MAXIMIZED);
    update_spec_t resync = {1, 1, 10, 20, 640, 480, false, 0, 0, NULL, false, 0, 0, WaddleWindowStateResizable | WaddleWindowStateMaximizable};
    Check(send_update(&resync) == 0);
    Check(reducer_synced(1, 1, NULL) == 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_UNSET_MAXIMIZED) == unmaximize + 1);

    /* Host-side state changes on a toplevel: an observation that adds minimized, maximized and fullscreen plans
     * set_minimized, set_maximized, set_fullscreen in that order; clearing them unsets maximized and fullscreen. */
    unsigned minimize = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MINIMIZED);
    unsigned maximize = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED);
    unsigned fullscreen = fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_FULLSCREEN);
    update_spec_t iconic = resync;
    iconic.state = WaddleWindowStateMinimized | WaddleWindowStateMaximized | WaddleWindowStateFullscreen | WaddleWindowStateMaximizable;
    unsigned mark = fake_request_count;
    Check(send_update(&iconic) == 0);
    Check(expect_signature(mark, "xdg_toplevel:13 xdg_toplevel:9 xdg_toplevel:11") == 0);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MINIMIZED) == minimize + 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_MAXIMIZED) == maximize + 1);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_SET_FULLSCREEN) == fullscreen + 1);
    uint32_t applied = 0;
    Check(reducer_synced(1, 1, &applied) == 1);
    Check((applied & WaddleWindowStateMaximized) != 0 && (applied & WaddleWindowStateFullscreen) != 0);
    mark = fake_request_count;
    update_spec_t restored = resync;
    Check(send_update(&restored) == 0);
    Check(expect_signature(mark, "xdg_toplevel:10 xdg_toplevel:12") == 0);

    /* An update for a window that is not realized is refused by the windows layer and counted nowhere. */
    unsigned updates = handler.counters.updates_applied;
    update_spec_t stray = resync;
    stray.window = 77;
    Check(send_update(&stray) == 0);
    Check(handler.counters.updates_applied == updates);

    /* Malformed frames are counted: an update without its state, and a state request without its state. */
    Check(handler.counters.frame_fields_refused == 0);
    Check(send_short_frame_as(InteropMsgWindowUpdate, 1, 1) == 0);
    Check(handler.counters.frame_fields_refused == 1);
    Check(send_short_frame_as(InteropMsgWindowStateRequest, 1, 1) == 0);
    Check(handler.counters.frame_fields_refused == 2);
    Check(handler.counters.updates_applied == updates);

    /* A window announced over a reducer that cannot be created: Activated is not a guest state bit, so the reducer
     * refuses it and the window is not realized. */
    announce_spec_t active = toplevel_spec(3, 1, "a-active");
    active.state = WaddleWindowStateActivated;
    Check(send_announce(&active) == 0);
    Check(!host_wayland_windows_contains(&handler.windows, 3, 1));
    Check(handler.counters.announces_refused >= 1);

    /* A duplicate pair is refused, and so is a menu whose parent is not realized (the windows layer refuses it). */
    unsigned refused = handler.counters.announces_refused;
    Check(send_announce(&capable) == 0);
    Check(handler.counters.announces_refused == refused + 1);
    announce_spec_t orphan = child_spec(5, 1, InteropRoleMenu, 42, 1, 1, 1, 10, 10);
    Check(send_announce(&orphan) == 0);
    Check(!host_wayland_windows_contains(&handler.windows, 5, 1));
    Check(handler.counters.announces_refused == refused + 2);

    /* Retire of the root realizes the subtree removal; the reducers of the removed windows are dropped. */
    announce_spec_t child = child_spec(2, 1, InteropRoleDialog, 1, 1, 0, 0, 300, 200);
    Check(announce_mapped(&child) == 0);
    Check(record_of(2, 1) != NULL && reducer_synced(2, 1, NULL) == 1);
    unsigned retires = handler.counters.retires_applied;
    Check(send_retire(1, 1) == 0);
    Check(handler.counters.retires_applied == retires + 1);
    Check(!host_wayland_windows_contains(&handler.windows, 1, 1));
    Check(!host_wayland_windows_contains(&handler.windows, 2, 1));
    Check(reducer_synced(1, 1, NULL) == -1 && reducer_synced(2, 1, NULL) == -1);

    /* A reducer that disappeared under a realized window (a white-box drop): configure and state requests still
     * complete for the guest, and only the host request is refused. */
    announce_spec_t lone = toplevel_spec(6, 1, "a-lone");
    Check(announce_mapped(&lone) == 0);
    unsigned lone_toplevel = index_of(record_of(6, 1)->toplevel);
    unsigned lone_surface = index_of(record_of(6, 1)->xdg_surface);
    for (uint32_t slot = 0; slot < SessionWindowsMaxReducers; ++slot) {
        if (handler.reducers[slot].used && handler.reducers[slot].window == 6) {
            Check(waddle_window_state_destroy(&handler.reducers[slot].handle) == WaddleWindowStateOk);
            handler.reducers[slot].used = 0;
        }
    }
    before = counters_now();
    fake_clear_recording();
    fake_queue_toplevel_configure(lone_toplevel, 300, 300, NULL, 0);
    fake_queue_surface_configure(lone_surface, 1);
    fake_queue_toplevel_close(lone_toplevel);
    Check(pump() == 0);
    Check(fake_frame_count == 2);
    Check(counters_now().configures_sent == before.configures_sent + 1);
    Check(counters_now().closes_sent == before.closes_sent + 1);
    Check(send_state_request(6, 1, 0) == 0);
    Check(counters_now().state_requests_ignored == before.state_requests_ignored + 1);

    /* Icons are ignored; the catalog lane owns them. */
    Check(send_icon_changed(6, 1) == 0);
    Check(counters_now().icons_ignored == before.icons_ignored + 1);
    Check(fake_frame_count == 2);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- API edges and the rest of the handler surface -------------------------------------------------- */

static unsigned seat_adds;
static unsigned seat_removes;

static void count_seat(void *user, struct wl_seat *seat, bool added) {
    (void)user;
    (void)seat;
    if (added) ++seat_adds;
    else ++seat_removes;
}

static int test_api_edges(void) {
    fake_begin();
    session_windows_handler_t local;
    session_windows_config_t config;
    memset(&config, 0, sizeof config);
    config.device = "dev0";
    config.display = NULL;
    Check(session_windows_handler_init(NULL, &config) == 1);
    Check(session_windows_handler_init(&local, NULL) == 1);
    session_windows_config_t bad = config;
    bad.device = NULL;
    Check(session_windows_handler_init(&local, &bad) == 1);
    bad.device = "";
    Check(session_windows_handler_init(&local, &bad) == 1);
    char long_device[66];
    memset(long_device, 'd', sizeof long_device - 1);
    long_device[sizeof long_device - 1] = '\0';
    bad.device = long_device;
    Check(session_windows_handler_init(&local, &bad) == 1);
    bad.device = "dev0";
    bad.scale_numerator = 3;
    bad.scale_denominator = 0;
    Check(session_windows_handler_init(&local, &bad) == 1);
    bad.scale_numerator = 0;
    bad.scale_denominator = 5;
    Check(session_windows_handler_init(&local, &bad) == 1);
    bad.scale_numerator = 9;
    bad.scale_denominator = 1;
    Check(session_windows_handler_init(&local, &bad) == 1);
    bad.scale_numerator = 16;
    bad.scale_denominator = 2;
    Check(session_windows_handler_init(&local, &bad) == 0 && local.scale == 8);
    bad.scale_numerator = 0;
    bad.scale_denominator = 0;
    long_device[64] = '\0';
    bad.device = long_device;
    Check(session_windows_handler_init(&local, &bad) == 0 && local.scale == 1);
    long_device[64] = 'd';

    /* A handler that never reached on_ready: every accessor is safe and reports the idle state. */
    session_handler_t idle = session_windows_handler_descriptor(&local);
    Check(idle.name && !strcmp(idle.name, "session_windows"));
    Check(idle.context == &local && idle.capabilities == InteropCapWindows);
    Check(idle.owns_type && idle.on_ready && idle.on_frame && idle.poll_fds && idle.on_fds && idle.on_tick && idle.next_deadline_ms && idle.on_closing);
    Check(local.lease_epoch == 0);
    session_windows_set_lease_epoch(NULL, 1);
    session_windows_handler_t raw;
    memset(&raw, 0, sizeof raw);
    session_windows_set_lease_epoch(&raw, 9);
    Check(raw.lease_epoch == 0);
    session_windows_set_lease_epoch(&local, 4);
    Check(local.lease_epoch == 4);
    session_windows_set_lease_epoch(&local, 0);
    host_wayland_press_input_t input;
    memset(&input, 0, sizeof input);
    input.protocol_serial = 1;
    input.seat = (struct wl_seat *)(void *)&input;
    input.window = 1;
    input.generation = 1;
    input.kind = HostWaylandPressKey;
    input.button = 30;
    Check(session_windows_record_press(&local, &input) == HostWaylandNotInitialized);
    Check(session_windows_record_press(NULL, &input) == HostWaylandNotInitialized);
    session_windows_release_press(NULL, 1);
    session_windows_release_press(&local, 1);
    session_windows_set_seat_listener(NULL, NULL, NULL);
    session_windows_set_seat_listener(&local, count_seat, NULL);
    session_windows_set_seat_listener(&local, NULL, NULL);
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 99;
    Check(idle.poll_fds(idle.context, fds, SessionHandlerMaxDescriptors, &count) == 0 && count == 0);
    Check(idle.next_deadline_ms(idle.context, 5) == UINT64_MAX);
    Check(idle.on_tick(idle.context, 5, FakeSender) == 0);
    Check(idle.on_fds(idle.context, fds, 0, 5, FakeSender) == 0);
    Check(idle.on_frame(idle.context, InteropMsgWindowAnnounce, 0, (const uint8_t *)"", 0, FakeSender) == 1);
    Check(idle.on_ready(idle.context, 0, FakeSender) == 0);
    idle.on_closing(idle.context, 0, FakeSender);
    Check(local.closing == 1);

    /* counters: NULL handler reads zeros, NULL output is ignored. */
    session_windows_counters_t counters;
    memset(&counters, 0xAB, sizeof counters);
    session_windows_handler_counters(NULL, &counters);
    Check(counters.frames_received == 0 && counters.announces_realized == 0 && counters.frame_fields_refused == 0);
    session_windows_handler_counters(&local, NULL);

    /* A ready session: owns_type, capacity, deadlines, request ids, unowned types and the seat listener. */
    Check(start_session(1, 1) == 0);
    session_handler_t live = table;
    static const uint32_t owned[] = {InteropMsgWindowAnnounce, InteropMsgWindowUpdate, InteropMsgWindowRetire, InteropMsgFrameReady,
        InteropMsgWindowMoveRequest, InteropMsgWindowResizeRequest, InteropMsgWindowStateRequest, InteropMsgWindowIconChanged};
    for (unsigned index = 0; index < sizeof owned / sizeof owned[0]; ++index) Check(live.owns_type(live.context, owned[index]) == 1);
    Check(live.owns_type(live.context, InteropMsgWindowConfigure) == 0);
    Check(live.owns_type(live.context, InteropMsgPing) == 0);
    Check(live.owns_type(live.context, InteropMsgInputKey) == 0);

    count = 99;
    Check(live.poll_fds(live.context, fds, 0, &count) == 0 && count == 0);
    fake_fd_value = -1;
    Check(live.poll_fds(live.context, fds, SessionHandlerMaxDescriptors, &count) == 1);
    fake_fd_value = FakeFd;
    Check(live.poll_fds(live.context, fds, SessionHandlerMaxDescriptors, &count) == 0 && count == 1 && fds[0].fd == FakeFd);
    Check(live.next_deadline_ms(live.context, 5) == UINT64_MAX);
    session_windows_set_seat_listener(&local, NULL, NULL);
    Check(handler.seat_listener == NULL);
    session_windows_set_seat_listener(&handler, count_seat, NULL);
    Check(handler.seat_listener == count_seat);

    /* A second on_ready on a ready session changes nothing. */
    unsigned creates = fake_creates;
    Check(live.on_ready(live.context, InteropCapWindows, FakeSender) == 0);
    Check(fake_creates == creates);

    /* Request ids other than zero are refused; the message types that are not owned are ignored. */
    uint8_t garbage[4] = {1, 2, 3, 4};
    Check(live.on_frame(live.context, InteropMsgWindowAnnounce, 9, garbage, sizeof garbage, FakeSender) == 1);
    Check(handler.failed == 0);
    Check(live.on_frame(live.context, InteropMsgPing, 0, garbage, sizeof garbage, FakeSender) == 0);
    /* Truncated frames of owned types are refused without a crash. */
    Check(live.on_frame(live.context, InteropMsgWindowAnnounce, 0, garbage, sizeof garbage, FakeSender) == 0);
    Check(live.on_frame(live.context, InteropMsgWindowMoveRequest, 0, garbage, 0, FakeSender) == 0);
    Check(handler.counters.frame_fields_refused == 2);

    /* Seat listener: removal is forwarded after the handler forgets the seat. */
    seat_adds = seat_removes = 0;
    session_windows_set_seat_listener(&handler, count_seat, NULL);
    fake_queue_remove(5);
    Check(pump() == 0);
    Check(seat_removes == 1 && seat_adds == 0);
    session_windows_set_seat_listener(&handler, NULL, NULL);

    /* A deadline exists only while frames are pending. */
    fake_send_mode = FakeSendFull;
    Check(send_frame_ready(1, 1, 0, 1) == 0);
    Check(live.next_deadline_ms(live.context, 500) == 600);
    fake_send_mode = FakeSendAccept;
    Check(tick() == 0);
    Check(live.next_deadline_ms(live.context, 500) == UINT64_MAX);

    /* Output that is not flushed yet: POLLOUT is requested, and nothing else changes. */
    fake_flush_mode = FakeEagain;
    Check(tick() == 0);
    Check(live.poll_fds(live.context, fds, 1, &count) == 0 && count == 1);
    fake_flush_mode = FakeOk;

    /* Closing with frames pending drops them, counted, and sends nothing. */
    fake_send_mode = FakeSendFull;
    Check(send_frame_ready(1, 1, 0, 2) == 0);
    Check(handler.pending_count == 1);
    unsigned attempts = fake_send_attempts;
    unsigned dropped = handler.counters.frames_dropped_draining;
    live.on_closing(live.context, 1, FakeSender);
    Check(fake_send_attempts == attempts);
    Check(handler.counters.frames_dropped_draining == dropped + 1 && handler.pending_count == 0);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- test 11: randomized sequences ---------------------------------------------------------------- */

static uint64_t rng_state;

/** @brief xorshift64* (Vigna). The seed is never zero. */
static uint64_t next_random(void) {
    uint64_t value = rng_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    rng_state = value;
    return value * 0x2545F4914F6CDD1DULL;
}

static uint32_t below(uint32_t bound) {
    return (uint32_t)(next_random() % bound);
}

static uint64_t random_generation;
static uint32_t random_serial_last;
static struct wl_seat *random_foreign_seat;
static unsigned random_frames_by_type[5];

/** @brief Tally of the accepted frames the random run checks, by type. */
static void count_frame_type(uint32_t type) {
    switch (type) {
    case InteropMsgWindowConfigure: ++random_frames_by_type[0]; break;
    case InteropMsgWindowClose: ++random_frames_by_type[1]; break;
    case InteropMsgPopupRepositioned: ++random_frames_by_type[2]; break;
    case InteropMsgPopupDismissed: ++random_frames_by_type[3]; break;
    case InteropMsgFrameRelease: ++random_frames_by_type[4]; break;
    default: break;
    }
}

typedef struct live_pair_t {
    uint64_t window;
    uint64_t generation;
    uint32_t role;
} live_pair_t;

static unsigned live_pairs(live_pair_t *out, unsigned capacity) {
    unsigned count = 0;
    for (uint32_t index = 0; index < HostWindowMapMaxRealized && count < capacity; ++index) {
        const host_wayland_record_t *rec = &handler.windows.records[index];
        if (rec->generation == 0) continue;
        out[count].window = rec->window;
        out[count].generation = rec->generation;
        out[count].role = rec->role;
        ++count;
    }
    return count;
}

/** @brief Pick a live window of one of the given roles, or report none. */
static int pick_pair(const live_pair_t *pairs, unsigned count, int toplevels_only, live_pair_t *out) {
    live_pair_t candidates[HostWindowMapMaxRealized];
    unsigned found = 0;
    for (unsigned index = 0; index < count; ++index) {
        int toplevel = pairs[index].role == InteropRoleToplevel || pairs[index].role == InteropRoleDialog || pairs[index].role == InteropRoleUacConsent;
        if (toplevels_only ? toplevel : !toplevel) candidates[found++] = pairs[index];
    }
    if (found == 0) return 0;
    *out = candidates[below(found)];
    return 1;
}

static void random_announce(const live_pair_t *pairs, unsigned count) {
    announce_spec_t spec = toplevel_spec(1 + below(12), 1 + random_generation, "a-rand");
    random_generation++;
    uint32_t roll = below(100);
    live_pair_t parent;
    if (roll < 45) {
        spec.role = InteropRoleToplevel;
    } else if (roll < 60 && count > 0) {
        spec.role = InteropRoleDialog;
        parent = pairs[below(count)];
        spec.has_parent = true;
        spec.parent = parent.window;
        spec.parent_generation = parent.generation;
    } else if (roll < 75 && count > 0) {
        spec.role = InteropRoleMenu;
        parent = pairs[below(count)];
        spec.has_parent = true;
        spec.parent = parent.window;
        spec.parent_generation = parent.generation;
    } else if (roll < 80 && count > 0) {
        spec.role = InteropRoleTooltip;
        parent = pairs[below(count)];
        spec.has_parent = true;
        spec.parent = parent.window;
        spec.parent_generation = parent.generation;
    } else if (roll < 90) {
        spec.role = InteropRoleUacConsent;
    } else {
        spec.role = InteropRoleToplevel;
    }
    spec.x = (int32_t)below(250) - 50;
    spec.y = (int32_t)below(250) - 50;
    spec.width = 1 + below(700);
    spec.height = 1 + below(500);
    spec.title = below(3) == 0 ? "random" : NULL;
    spec.has_min_size = below(4) == 0;
    spec.min_width = below(200);
    spec.min_height = below(200);
    spec.state = below(128) & 0x77u;
    if (spec.role == InteropRoleMenu && below(4) == 0) {
        spec.has_serial = 1;
        spec.serial = random_serial_last ? 1 + random_serial_last % 64 : 1;
    }
    /* Duplicates of a live pair are refused by the table; the frame is still sent. */
    if (below(10) == 0 && count > 0) {
        spec.window = pairs[0].window;
        spec.generation = pairs[0].generation;
    }
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, &spec);
    if (length) send_frame(InteropMsgWindowAnnounce, frame, length);
    host_wayland_record_t *rec = record_of(spec.window, spec.generation);
    if (rec && rec->decoration) fake_queue_decoration_configure(index_of(rec->decoration), below(10) == 0 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void random_update(const live_pair_t *pairs, unsigned count) {
    if (count == 0) return;
    live_pair_t target = pairs[below(count)];
    update_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.window = target.window;
    spec.generation = target.generation;
    spec.x = (int32_t)below(250) - 50;
    spec.y = (int32_t)below(250) - 50;
    spec.width = 1 + below(700);
    spec.height = 1 + below(500);
    spec.state = below(128) & 0x77u;
    spec.title = below(4) == 0 ? "renamed" : NULL;
    spec.has_min_size = below(5) == 0;
    spec.min_width = below(200);
    spec.min_height = below(200);
    if (target.role == InteropRoleToplevel && below(4) == 0 && count > 0) {
        live_pair_t parent;
        if (pick_pair(pairs, count, 1, &parent) && (parent.window != target.window || parent.generation != target.generation)) {
            spec.has_parent = true;
            spec.parent = parent.window;
            spec.parent_generation = parent.generation;
        }
    }
    send_update(&spec);
}

static void random_configure(const live_pair_t *pairs, unsigned count) {
    live_pair_t target;
    if (!pick_pair(pairs, count, 1, &target)) return;
    host_wayland_record_t *rec = record_of(target.window, target.generation);
    if (!rec || !rec->toplevel || !rec->xdg_surface) return;
    static const uint32_t bits[] = {XDG_TOPLEVEL_STATE_ACTIVATED, XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_FULLSCREEN};
    uint32_t states[3];
    size_t state_count = 0;
    for (unsigned index = 0; index < 3; ++index) {
        if (below(2)) states[state_count++] = bits[index];
    }
    static const int32_t sizes[] = {0, 1, 320, 700, 20000};
    int32_t width = sizes[below(5)];
    int32_t height = below(4) == 0 ? 0 : sizes[1 + below(4)];
    if (rec->decoration) fake_queue_decoration_configure(index_of(rec->decoration), ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    fake_queue_toplevel_configure(index_of(rec->toplevel), width, height, states, state_count);
    fake_queue_surface_configure(index_of(rec->xdg_surface), 1 + below(1000));
}

static void random_close(const live_pair_t *pairs, unsigned count) {
    live_pair_t target;
    if (!pick_pair(pairs, count, 1, &target)) return;
    host_wayland_record_t *rec = record_of(target.window, target.generation);
    if (rec && rec->toplevel) fake_queue_toplevel_close(index_of(rec->toplevel));
}

static void random_popup(const live_pair_t *pairs, unsigned count) {
    live_pair_t target;
    if (!pick_pair(pairs, count, 0, &target)) return;
    host_wayland_record_t *rec = record_of(target.window, target.generation);
    if (!rec || !rec->popup) return;
    unsigned popup = index_of(rec->popup);
    switch (below(3)) {
    case 0:
        fake_queue_popup_configure(popup, (int32_t)below(200) - 20, (int32_t)below(200) - 20, 1 + (int32_t)below(100), 1 + (int32_t)below(60));
        break;
    case 1:
        fake_queue_popup_repositioned(popup, below(4));
        break;
    default:
        fake_queue_popup_done(popup);
        break;
    }
}

static void random_press(const live_pair_t *pairs, unsigned count) {
    uint32_t serial = 1 + below(64);
    live_pair_t target = {1 + below(12), 1 + below(random_generation + 1), 0};
    if (count > 0 && below(5) != 0) target = pairs[below(count)];
    struct wl_seat *seat = below(7) == 0 ? random_foreign_seat : session_seat();
    uint32_t button = below(10) < 7 ? SessionWindowsButtonLeft : 0x111u;
    host_wayland_press_input_t input;
    memset(&input, 0, sizeof input);
    input.protocol_serial = serial;
    input.wayland_serial = 1000 + serial;
    input.seat = seat;
    input.window = target.window;
    input.generation = target.generation;
    input.lease_epoch = below(3);
    input.kind = below(8) == 0 ? HostWaylandPressTouch : HostWaylandPressPointer;
    input.button = button;
    input.now_ms = test_now;
    session_windows_record_press(&handler, &input);
    random_serial_last = serial;
}

static void random_move_resize(const live_pair_t *pairs, unsigned count) {
    uint32_t serial = 1 + below(64);
    live_pair_t target = {1 + below(12), 1 + below(random_generation + 1), 0};
    if (count > 0 && below(4) != 0) target = pairs[below(count)];
    if (below(2)) {
        send_move(target.window, target.generation, serial);
    } else {
        send_resize(target.window, target.generation, serial, below(12));
    }
}

static void random_state(const live_pair_t *pairs, unsigned count) {
    if (count == 0) return;
    live_pair_t target = pairs[below(count)];
    send_state_request(target.window, target.generation, below(128));
}

static void random_frame_ready(const live_pair_t *pairs, unsigned count) {
    live_pair_t target = {1 + below(12), 1 + below(random_generation + 1), 0};
    if (count > 0 && below(3) != 0) target = pairs[below(count)];
    send_frame_ready(target.window, target.generation, below(4), next_random() & 0xFFFFFFu);
}

static void random_release(void) {
    session_windows_release_press(&handler, 1 + below(64));
}

/** @brief One operation of the random mix. The pump or tick that follows delivers whatever it queued. */
static void random_operation(void) {
    live_pair_t pairs[HostWindowMapMaxRealized];
    unsigned count = live_pairs(pairs, HostWindowMapMaxRealized);
    uint32_t roll = below(100);
    if (roll < 10) random_announce(pairs, count);
    else if (roll < 18) random_update(pairs, count);
    else if (roll < 23) {
        if (count > 0) {
            live_pair_t target = pairs[below(count)];
            send_retire(target.window, target.generation);
        }
    } else if (roll < 33) random_configure(pairs, count);
    else if (roll < 37) random_close(pairs, count);
    else if (roll < 42) random_popup(pairs, count);
    else if (roll < 52) random_press(pairs, count);
    else if (roll < 56) random_release();
    else if (roll < 62) random_move_resize(pairs, count);
    else if (roll < 65) random_state(pairs, count);
    else if (roll < 68) random_frame_ready(pairs, count);
    else if (roll < 70) session_windows_set_lease_epoch(&handler, below(3));
    else if (roll < 74) {
        uint32_t mode = below(100);
        fake_send_mode = mode < 70 ? FakeSendAccept : mode < 95 ? FakeSendFull : FakeSendDraining;
    } else if (roll < 80) test_now += below(150);
}

/** @brief The invariants of 11, checked after every step. */
static int random_check(unsigned seed, unsigned step, unsigned protocol_before, unsigned order_before) {
    if (fake_protocol_errors != protocol_before || fake_order_violations != order_before) {
        fprintf(stderr, "seed %u step %u: double reported a protocol or order error\n", seed, step);
        return 1;
    }
    Check(!handler.failed);
    Check(handler.pending_count <= SessionWindowsMaxPending);
    for (unsigned index = 0; index < fake_frame_count; ++index) {
        outbound_t out;
        Check(decode_outbound(fake_frames[index], fake_frame_length[index], &out) == 0);
        Check(out.request_id == 0);
        if (out.type == InteropMsgWindowConfigure || out.type == InteropMsgWindowClose || out.type == InteropMsgPopupRepositioned || out.type == InteropMsgPopupDismissed) {
            if (!host_wayland_windows_contains(&handler.windows, out.window, out.generation)) {
                fprintf(stderr, "seed %u step %u: frame type %u names an unrealized window %llu/%llu\n", seed, step, out.type,
                    (unsigned long long)out.window, (unsigned long long)out.generation);
                return 1;
            }
        }
        count_frame_type(out.type);
    }
    for (uint32_t index = 0; index < handler.pending_count; ++index) {
        uint32_t type = handler.pending[index].type;
        if (type == InteropMsgWindowConfigure || type == InteropMsgWindowClose || type == InteropMsgPopupRepositioned || type == InteropMsgPopupDismissed) {
            if (!host_wayland_windows_contains(&handler.windows, handler.pending[index].window, handler.pending[index].generation)) {
                fprintf(stderr, "seed %u step %u: pending frame type %u names an unrealized window %llu/%llu\n", seed, step, type,
                    (unsigned long long)handler.pending[index].window, (unsigned long long)handler.pending[index].generation);
                return 1;
            }
        }
    }
    return 0;
}

static int run_random_segment(uint64_t seed, uint32_t numerator, uint32_t denominator, unsigned steps) {
    rng_state = seed;
    random_generation = 0;
    random_serial_last = 0;
    static struct wl_seat *const foreign = (struct wl_seat *)(void *)&foreign;
    random_foreign_seat = foreign;
    memset(random_frames_by_type, 0, sizeof random_frames_by_type);
    Check(start_session_with(numerator, denominator, GlobalAll, InteropCapWindows) == 0);
    fake_clear_recording();
    for (unsigned step = 0; step < steps; ++step) {
        fake_clear_recording();
        unsigned protocol_before = fake_protocol_errors;
        unsigned order_before = fake_order_violations;
        if (handler.pending_count >= 100) fake_send_mode = FakeSendAccept;
        random_operation();
        if (handler.pending_count >= 100) fake_send_mode = FakeSendAccept;
        int result = (fake_event_tail > fake_event_head || below(2)) ? pump() : tick();
        if (result != 0) {
            fprintf(stderr, "seed %llu step %u: dispatch failed\n", (unsigned long long)seed, step);
            return 1;
        }
        Check(random_check((unsigned)seed, step, protocol_before, order_before) == 0);
        if (fake_send_mode == FakeSendDraining) fake_send_mode = FakeSendAccept;
    }
    /* Final: close the session with whatever is left and check the balance of proxies. */
    fake_send_mode = FakeSendAccept;
    unsigned protocol_end = fake_protocol_errors;
    table.on_closing(table.context, 0, FakeSender);
    Check(fake_protocol_errors == protocol_end);
    Check(fake_live_count() == 1);
    Check(fake_creates - fake_destroys == 1);
    Check(fake_order_violations == 0);
    Check(end_session() == 0);
    fprintf(stdout, "random seed 0x%llx: %u steps, frames: configure %u close %u popup_repositioned %u popup_dismissed %u frame_release %u\n",
        (unsigned long long)seed, steps, random_frames_by_type[0], random_frames_by_type[1], random_frames_by_type[2], random_frames_by_type[3], random_frames_by_type[4]);
    return 0;
}

static int test_random_sequences(void) {
    Check(run_random_segment(0x9E3779B97F4A7C15ULL, 1, 1, 3000) == 0);
    Check(run_random_segment(0xD1B54A32D192ED03ULL, 2, 1, 3000) == 0);
    Check(run_random_segment(0x2545F4914F6CDD1DULL, 3, 2, 3000) == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- bug regressions ------------------------------------------------------------------------------ */

/**
 * @brief A frame queued while the feature queue is full names a window that the guest then retires. The frame must not
 * be sent after the retire: every window-scoped frame in the pending list names a realized window (4.3).
 */
static int test_pending_frames_of_retired_windows(void) {
    Check(start_session(1, 1) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_mapped(&root) == 0);
    announce_spec_t child = child_spec(2, 1, InteropRoleDialog, 1, 1, 0, 0, 300, 200);
    Check(announce_mapped(&child) == 0);
    unsigned root_toplevel = index_of(record_of(1, 1)->toplevel);
    unsigned root_surface = index_of(record_of(1, 1)->xdg_surface);
    unsigned child_toplevel = index_of(record_of(2, 1)->toplevel);
    fake_clear_recording();

    fake_send_mode = FakeSendFull;
    fake_queue_toplevel_close(child_toplevel);
    fake_queue_toplevel_configure(root_toplevel, 500, 400, NULL, 0);
    fake_queue_surface_configure(root_surface, 1);
    Check(pump() == 0);
    Check(handler.pending_count == 2);

    /* Retiring the root removes the dialog too; both pending frames name windows that are gone. */
    Check(send_retire(1, 1) == 0);
    Check(!host_wayland_windows_contains(&handler.windows, 1, 1));
    Check(!host_wayland_windows_contains(&handler.windows, 2, 1));
    fake_send_mode = FakeSendAccept;
    Check(tick() == 0);
    Check(handler.pending_count == 0);
    Check(fake_frame_count == 0);
    Check(end_session() == 0);
    Check(check_double_clean() == 0);
    return 0;
}

/* ---- runner ---------------------------------------------------------------------------------------- */

typedef int (*test_fn)(void);

static unsigned tests_run;
static unsigned tests_failed;

static void run_test(const char *name, test_fn fn) {
    ++tests_run;
    int result = fn();
    if (handler.initialized && !handler.closing) table.on_closing(table.context, 0, FakeSender);
    if (result == 0 && check_double_clean() != 0) result = 1;
    if (result != 0) {
        ++tests_failed;
        fprintf(stderr, "FAIL %s\n", name);
    } else {
        printf("PASS %s\n", name);
    }
    memset(&handler, 0, sizeof handler);
}

int main(void) {
    run_test("announce requests", test_announce_requests);
    run_test("configure, close and popup events", test_event_frames);
    run_test("configure rects at scale 2 and the limit", test_configure_scaled);
    run_test("move and resize serial matrix", test_move_resize_matrix);
    run_test("move after the seat is removed", test_move_forgotten_seat);
    run_test("state requests", test_state_requests);
    run_test("frame ready releases the slot", test_frame_ready);
    run_test("backpressure coalescing", test_backpressure_coalesce);
    run_test("backpressure closes and overflow", test_backpressure_closes_and_overflow);
    run_test("backpressure draining and failure", test_backpressure_draining_and_fail);
    run_test("stale generations", test_stale_generations);
    run_test("display error on read", test_display_error_read);
    run_test("display error on dispatch", test_display_error_dispatch);
    run_test("display flush errors", test_display_flush_errors);
    run_test("teardown order", test_teardown_order);
    run_test("missing globals", test_missing_globals);
    run_test("reducer paths and window lifecycle", test_reducer_paths);
    run_test("API edges", test_api_edges);
    run_test("randomized sequences", test_random_sequences);
    run_test("bug: pending frames of retired windows", test_pending_frames_of_retired_windows);
    printf("session windows handler tests: %u run, %u failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
