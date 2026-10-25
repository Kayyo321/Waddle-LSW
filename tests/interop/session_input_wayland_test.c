/**
 * @file session_input_wayland_test.c
 * @brief Tests for the Wayland keyboard glue (session_input_wayland.c) against the real host
 *        keyboard handler (session_input_handler.c).
 * @note Built with -Wl,--wrap for interop_host_feature_send and clock_gettime (as the handler
 *       test does), and for wl_proxy_add_listener, wl_proxy_marshal_flags and wl_proxy_get_version.
 *       The wrapped libwayland entry points capture listener tables and record inhibitor requests;
 *       the test then invokes the captured callbacks directly, which is how the real listeners run.
 *       Handler observations read the handler state fields (focused, forwarding, pending, ...).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "keyboard_shortcuts_inhibit_client.h"
#include "keymap_names.h"
#include "session_input_handler.h"
#include "session_input_wayland.h"
#include "waddle/interop_protocol.h"

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

enum {
    FrameCapacity = 160,
    MaxListeners = 64,
    MaxInhibitorTokens = 64,
    MaxTestSurfaces = 80,
    RandomSteps = 1000,
    RandomSurfaces = 8
};

/* ------------------------------------------------------------------------- */
/* Handler wraps (same contract as session_input_handler_test.c)             */
/* ------------------------------------------------------------------------- */

static uint8_t g_last_frame[FrameCapacity];
static size_t g_last_length;
static size_t g_frames_sent;
static char g_sender_token;
static interop_host_feature_sender_t *const g_sender = (interop_host_feature_sender_t *)&g_sender_token;
static uint64_t g_now_ns = 5000000000ull;

/** @brief Records the last frame; the handshake reads the LeaseAck serial from it. */
int __wrap_interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length) {
    (void)sender;
    CHECK(length <= FrameCapacity);
    memcpy(g_last_frame, frame, length);
    g_last_length = length;
    g_frames_sent++;
    return HostFeatureSuccess;
}

/** @brief Deterministic monotonic clock. */
int __wrap_clock_gettime(clockid_t clock, struct timespec *ts) {
    (void)clock;
    ts->tv_sec = (time_t)(g_now_ns / 1000000000ull);
    ts->tv_nsec = (long)(g_now_ns % 1000000000ull);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* libwayland wraps: listener capture, inhibitor requests, proxy version     */
/* ------------------------------------------------------------------------- */

typedef struct captured_listener_t {
    const void *proxy;
    void (**implementation)(void);
    void *data;
} captured_listener_t;

static captured_listener_t g_listeners[MaxListeners];
static size_t g_listener_count;
static int g_listener_fail;

/** @brief Captures the listener table and data for a proxy; fails when g_listener_fail is set. */
int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    if (g_listener_fail) return -1;
    CHECK(g_listener_count < MaxListeners);
    g_listeners[g_listener_count].proxy = proxy;
    g_listeners[g_listener_count].implementation = implementation;
    g_listeners[g_listener_count].data = data;
    g_listener_count++;
    return 0;
}

static const captured_listener_t *find_listener(const void *proxy) {
    for (size_t index = g_listener_count; index > 0; index--) {
        if (g_listeners[index - 1].proxy == proxy) return &g_listeners[index - 1];
    }
    return NULL;
}

static char g_inhibitor_tokens[MaxInhibitorTokens];
static size_t g_inhibitor_next;
static int g_inhibit_fail;
static size_t g_inhibit_calls;
static size_t g_destroy_calls;
static const void *g_last_inhibit_manager;
static const void *g_last_inhibit_surface;
static const void *g_last_inhibit_seat;
static const void *g_last_destroyed;

/**
 * @brief Records inhibit_shortcuts (opcode 1 on the manager) and inhibitor destroy (destructor flag).
 *        The inhibit call returns a fresh token, or NULL when g_inhibit_fail is set.
 */
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface,
    uint32_t version, uint32_t flags, ...) {
    (void)interface;
    (void)version;
    if (flags & WL_MARSHAL_FLAG_DESTROY) {
        g_destroy_calls++;
        g_last_destroyed = proxy;
        return NULL;
    }
    CHECK(opcode == ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_INHIBIT_SHORTCUTS);
    g_inhibit_calls++;
    g_last_inhibit_manager = proxy;
    va_list args;
    va_start(args, flags);
    (void)va_arg(args, void *);
    g_last_inhibit_surface = va_arg(args, void *);
    g_last_inhibit_seat = va_arg(args, void *);
    va_end(args);
    if (g_inhibit_fail || g_inhibitor_next >= MaxInhibitorTokens) return NULL;
    return (struct wl_proxy *)&g_inhibitor_tokens[g_inhibitor_next++];
}

/** @brief Every proxy reports version 1; the glue passes it through unchanged. */
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
    (void)proxy;
    return 1u;
}

/* ------------------------------------------------------------------------- */
/* Fixture objects                                                           */
/* ------------------------------------------------------------------------- */

static char g_keyboard_token;
static char g_manager_token;
static char g_seat_token;
static char g_surface_tokens[MaxTestSurfaces];

static struct wl_keyboard *keyboard(void) { return (struct wl_keyboard *)&g_keyboard_token; }
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager(void) {
    return (struct zwp_keyboard_shortcuts_inhibit_manager_v1 *)&g_manager_token;
}
static struct wl_seat *seat(void) { return (struct wl_seat *)&g_seat_token; }
static struct wl_surface *surface_at(size_t index) { return (struct wl_surface *)&g_surface_tokens[index]; }

/** @brief Keymap with Caps (index 1), Num (4) and Scroll (7) resolved, plus layout us. */
static const char KeymapFixture[] = "xkb_symbols \"x\" {\n    include \"pc+us+de:2+inet(evdev)\"\n};\n"
    "xkb_compat \"x\" {\n    modifier_map Mod2 { <NMLK> };\n    modifier_map Mod5 { <SCLK> };\n    modifier_map Lock { <CAPS> };\n};\n";
/** @brief Keymap without Caps: parses, but would change caps to WaddleKeymapModifierAbsent if delivered. */
static const char KeymapNumOnly[] = "xkb_compat \"x\" {\n    modifier_map Mod2 { <NMLK> };\n};\n";
/** @brief Keymap whose layout name is rejected by the parser (upper case). */
static const char KeymapMalformed[] = "xkb_symbols \"x\" {\n    include \"pc+US\"\n};\n";

/** @brief Helper value: a window identity used across tests. */
enum { WindowA = 0x1001, WindowB = 0x1002 };

/* ------------------------------------------------------------------------- */
/* Handler lease handshake (copied from session_input_handler_test.c)        */
/* ------------------------------------------------------------------------- */

static uint64_t field_of(const uint8_t *frame, size_t length, uint16_t tag, uint32_t width) {
    uint64_t value = 0;
    CHECK(interop_field_uint(frame + InteropHeaderBytes, length - InteropHeaderBytes, tag, width, &value) == InteropErrorNone);
    return value;
}

static size_t build_guest_lease(uint8_t *out, uint16_t kind, int has_serial, uint32_t serial, uint64_t epoch) {
    interop_writer_t writer;
    interop_writer_init(&writer, out, FrameCapacity);
    if (has_serial) CHECK(interop_put_uint(&writer, InteropTagSerial, serial) == InteropErrorNone);
    CHECK(interop_put_uint(&writer, InteropTagLeaseEpoch, epoch) == InteropErrorNone);
    size_t length = 0;
    CHECK(interop_writer_finish(&writer, kind, 0u, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

static int deliver_lease(session_handler_t *handler, uint16_t kind, int has_serial, uint32_t serial, uint64_t epoch) {
    uint8_t frame[FrameCapacity];
    size_t length = build_guest_lease(frame, kind, has_serial, serial, epoch);
    return handler->on_frame(handler->context, kind, 0u, frame, length, g_sender);
}

/** @brief Revoked(epoch) then Ready(serial): leaves the handler in SessionInputReady. */
static void handshake(session_handler_t *handler, uint64_t epoch) {
    CHECK(deliver_lease(handler, InteropMsgLeaseRevoked, 0, 0u, epoch) == 0);
    CHECK(g_last_length > 0);
    CHECK(interop_header_decode(g_last_frame, g_last_length, &(interop_header_t){0}) == InteropErrorNone);
    uint32_t serial = (uint32_t)field_of(g_last_frame, g_last_length, InteropTagSerial, 4u);
    CHECK(deliver_lease(handler, InteropMsgLeaseReady, 1, serial, epoch) == 0);
}

/* ------------------------------------------------------------------------- */
/* Fixture: glue + handler in Ready with the keyboard attached               */
/* ------------------------------------------------------------------------- */

typedef struct fixture_t {
    session_input_wayland_t glue;
    session_input_t input;
    session_handler_t handler;
} fixture_t;

static void reset_counters(void) {
    g_listener_count = 0;
    g_listener_fail = 0;
    g_inhibitor_next = 0;
    g_inhibit_fail = 0;
    g_inhibit_calls = 0;
    g_destroy_calls = 0;
    g_last_inhibit_manager = NULL;
    g_last_inhibit_surface = NULL;
    g_last_inhibit_seat = NULL;
    g_last_destroyed = NULL;
    g_frames_sent = 0;
}

/** @brief Initializes glue and handler, completes the handshake, and attaches the keyboard. */
static void fixture_begin(fixture_t *fixture, uint8_t mode, struct zwp_keyboard_shortcuts_inhibit_manager_v1 *mgr) {
    reset_counters();
    CHECK(session_input_wayland_init(&fixture->glue, &fixture->input, mode, mgr, mgr != NULL ? seat() : NULL) ==
        SessionInputWaylandOk);
    fixture->handler = session_input_make_handler(&fixture->input);
    CHECK(fixture->handler.on_ready(fixture->handler.context, InteropCapInput | InteropCapKeyboardLayout, g_sender) == 0);
    handshake(&fixture->handler, 1u);
    CHECK(fixture->input.phase == SessionInputReady);
    CHECK(session_input_attach_keyboard(&fixture->glue, keyboard()) == SessionInputWaylandOk);
}

static void fixture_end(fixture_t *fixture) {
    fixture->handler.on_closing(fixture->handler.context, HostControlSuccess, g_sender);
    session_input_wayland_destroy(&fixture->glue);
}

/* ------------------------------------------------------------------------- */
/* wl_keyboard and inhibitor event shims (invoke the captured listeners)     */
/* ------------------------------------------------------------------------- */

static const struct wl_keyboard_listener *keyboard_table(void **data) {
    const captured_listener_t *captured = find_listener(keyboard());
    CHECK(captured != NULL);
    *data = captured->data;
    return (const struct wl_keyboard_listener *)captured->implementation;
}

static void ev_enter(struct wl_surface *surface) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->enter(data, keyboard(), 1u, surface, NULL);
}

static void ev_leave(struct wl_surface *surface) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->leave(data, keyboard(), 2u, surface);
}

static void ev_key(uint32_t key, uint32_t state) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->key(data, keyboard(), 3u, 100u, key, state);
}

static void ev_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->modifiers(data, keyboard(), 4u, depressed, latched, locked, 0u);
}

static void ev_repeat(int32_t rate, int32_t delay) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->repeat_info(data, keyboard(), rate, delay);
}

static void ev_keymap(uint32_t format, int fd, uint32_t size) {
    void *data = NULL;
    const struct wl_keyboard_listener *table = keyboard_table(&data);
    table->keymap(data, keyboard(), format, fd, size);
}

static void ev_inhibitor(struct zwp_keyboard_shortcuts_inhibitor_v1 *object, int active) {
    const captured_listener_t *captured = find_listener(object);
    CHECK(captured != NULL);
    const struct zwp_keyboard_shortcuts_inhibitor_v1_listener *table =
        (const struct zwp_keyboard_shortcuts_inhibitor_v1_listener *)captured->implementation;
    if (active) {
        table->active(captured->data, object);
    } else {
        table->inactive(captured->data, object);
    }
}

/** @brief Creates a memfd holding keymap text plus its trailing NUL, as a compositor sends it. */
static int make_keymap_fd(const char *text, size_t *out_size) {
    size_t length = strlen(text) + 1u;
    int fd = memfd_create("keymap", 0);
    CHECK(fd >= 0);
    CHECK(write(fd, text, length) == (ssize_t)length);
    *out_size = length;
    return fd;
}

/** @brief True when fd is no longer open in this process. */
static int fd_closed(int fd) {
    errno = 0;
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

static const session_input_pending_t *last_pending(const session_input_t *input) {
    CHECK(input->pending_count > 0);
    return &input->pending[input->pending_count - 1];
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_init_and_argument_errors(void) {
    reset_counters();
    session_input_wayland_t glue;
    session_input_t input;
    CHECK(session_input_wayland_init(NULL, &input, WaddleKeyboardInhibitFocused, NULL, NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_wayland_init(&glue, NULL, WaddleKeyboardInhibitFocused, NULL, NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_wayland_init(&glue, &input, WaddleKeyboardInhibitFocused, manager(), NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_wayland_init(&glue, &input, 9u, NULL, NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_wayland_init(&glue, &input, WaddleKeyboardInhibitNever, NULL, NULL) == SessionInputWaylandOk);
    CHECK(session_input_last_post_status(&glue) == SessionInputPostOk);

    CHECK(session_input_attach_keyboard(NULL, keyboard()) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_attach_keyboard(&glue, NULL) == SessionInputWaylandInvalidArgument);
    g_listener_fail = 1;
    CHECK(session_input_attach_keyboard(&glue, keyboard()) == SessionInputWaylandListenerFailed);
    CHECK(glue.keyboard == NULL);
    g_listener_fail = 0;
    CHECK(session_input_attach_keyboard(&glue, keyboard()) == SessionInputWaylandOk);
    CHECK(session_input_attach_keyboard(&glue, keyboard()) == SessionInputWaylandAlreadyAttached);

    CHECK(session_input_map_surface(NULL, surface_at(0), WindowA, 1u) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_map_surface(&glue, NULL, WindowA, 1u) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_map_surface(&glue, surface_at(0), 0u, 1u) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_map_surface(&glue, surface_at(0), WindowA, 0u) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_map_surface(&glue, surface_at(0), WindowA, 0x100000000ull) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_unmap_surface(NULL, surface_at(0)) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_unmap_surface(&glue, NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_unmap_surface(&glue, surface_at(0)) == SessionInputWaylandOk);

    CHECK(session_input_post_surface_state(NULL, WindowA, 1u, true, false) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_post_surface_state(&glue, WindowA, 0x100000000ull, true, false) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_post_surface_state(&glue, 0u, 1u, true, false) == SessionInputWaylandHandlerRejected);
    CHECK(session_input_last_post_status(&glue) == SessionInputPostInvalidArgument);

    CHECK(session_input_detach_keyboard(NULL) == SessionInputWaylandInvalidArgument);
    CHECK(session_input_detach_keyboard(&glue) == SessionInputWaylandOk);
    CHECK(session_input_detach_keyboard(&glue) == SessionInputWaylandNotAttached);
    session_input_wayland_destroy(&glue);
    CHECK(glue.input == NULL);
}

static void test_keymap_delivery(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitNever, NULL);

    size_t size = 0;
    int fd = make_keymap_fd(KeymapFixture, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    CHECK(fd_closed(fd));
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostOk);
    CHECK(fixture.input.indices.caps == 1u);
    CHECK(fixture.input.indices.num == 4u);
    CHECK(fixture.input.indices.scroll == 7u);
    CHECK(fixture.input.klid_known == 1u);

    /* Rejected format: fd closed, keymap not delivered (NumOnly would set caps to absent). */
    fd = make_keymap_fd(KeymapNumOnly, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, (uint32_t)size);
    CHECK(fd_closed(fd));
    CHECK(fixture.input.indices.caps == 1u);

    /* Zero size and oversize: rejected before mmap; fd still closed. */
    fd = make_keymap_fd(KeymapNumOnly, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 0u);
    CHECK(fd_closed(fd));
    fd = make_keymap_fd(KeymapNumOnly, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)WaddleKeymapMaxBytes + 1u);
    CHECK(fd_closed(fd));
    CHECK(fixture.input.indices.caps == 1u);

    /* mmap failure: a pipe cannot be mapped. */
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    close(pipe_fds[1]);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, pipe_fds[0], 16u);
    CHECK(fd_closed(pipe_fds[0]));
    CHECK(fixture.input.indices.caps == 1u);

    /* Parse rejection while focused: the handler stops forwarding and reports the status. */
    session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u);
    ev_enter(surface_at(0));
    CHECK(fixture.input.forwarding == 1u);
    fd = make_keymap_fd(KeymapMalformed, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    CHECK(fd_closed(fd));
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostKeymapInvalid);
    CHECK(fixture.input.forwarding == 0u);
    CHECK(fixture.input.focused == 1u);
    fixture_end(&fixture);
}

static void test_focus_and_keys(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitNever, NULL);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 2u) == SessionInputWaylandOk);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(1), WindowB, 3u) == SessionInputWaylandOk);

    /* Enter a known surface: physical focus and forwarding start. */
    ev_enter(surface_at(0));
    CHECK(fixture.input.focused == 1u);
    CHECK(fixture.input.focus_window == WindowA);
    CHECK(fixture.input.focus_generation == 2u);
    CHECK(fixture.input.forwarding == 1u);
    CHECK(last_pending(&fixture.input)->kind == SessionInputPendingFocus);
    CHECK(last_pending(&fixture.input)->pressed == 1u);

    /* Key down and up carry the evdev code as pressed 1 and 0. */
    size_t before = fixture.input.pending_count;
    ev_key(30u, WL_KEYBOARD_KEY_STATE_PRESSED);
    CHECK(fixture.input.pending_count == before + 1u);
    CHECK(last_pending(&fixture.input)->kind == SessionInputPendingKey);
    CHECK(last_pending(&fixture.input)->code == 30u);
    CHECK(last_pending(&fixture.input)->pressed == 1u);
    ev_key(30u, WL_KEYBOARD_KEY_STATE_RELEASED);
    CHECK(last_pending(&fixture.input)->code == 30u);
    CHECK(last_pending(&fixture.input)->pressed == 0u);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostOk);

    /* An unknown key state is not a protocol value; nothing is posted. */
    before = fixture.input.pending_count;
    ev_key(31u, 7u);
    CHECK(fixture.input.pending_count == before);
    /* Evdev code zero is outside 1..767: the handler rejects it and the glue records it. */
    ev_key(0u, WL_KEYBOARD_KEY_STATE_PRESSED);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostInvalidKey);

    /* Enter an unknown surface while focused: focus leaves. */
    ev_enter(NULL);
    CHECK(fixture.input.focused == 0u);
    CHECK(fixture.glue.focused_surface == NULL);
    CHECK(last_pending(&fixture.input)->kind == SessionInputPendingFocus);
    CHECK(last_pending(&fixture.input)->pressed == 0u);

    /* Enter an unknown surface while not focused: no handler effect. */
    before = fixture.input.pending_count;
    ev_enter(surface_at(5));
    CHECK(fixture.input.focused == 0u);
    CHECK(fixture.input.pending_count == before);

    /* Leave after a focus: physical focus is cleared. */
    ev_enter(surface_at(1));
    CHECK(fixture.input.focus_window == WindowB);
    ev_leave(surface_at(1));
    CHECK(fixture.input.focused == 0u);
    CHECK(fixture.glue.focused_surface == NULL);

    /* Enter a known surface while focused on another: the handler moves focus to it. */
    ev_enter(surface_at(0));
    ev_enter(surface_at(1));
    CHECK(fixture.input.focus_window == WindowB);
    CHECK(fixture.input.focus_generation == 3u);
    CHECK(fixture.glue.focused_surface == surface_at(1));

    /* Keys while not forwarding (no focus) are accepted and ignored by the handler. */
    ev_leave(surface_at(1));
    before = fixture.input.pending_count;
    ev_key(30u, WL_KEYBOARD_KEY_STATE_PRESSED);
    CHECK(fixture.input.pending_count == before);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostOk);

    /* Leave when nothing is focused is a handler no-op. */
    ev_leave(surface_at(0));
    CHECK(fixture.input.focused == 0u);
    fixture_end(&fixture);
}

static void test_modifiers_and_repeat(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitNever, NULL);
    size_t size = 0;
    int fd = make_keymap_fd(KeymapFixture, &size);
    ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    CHECK(fd_closed(fd));
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    size_t before = fixture.input.pending_count;
    ev_modifiers(0u, 0u, 1u << fixture.input.indices.caps);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostOk);
    CHECK(fixture.input.pending_count == before + 1u);
    CHECK(last_pending(&fixture.input)->kind == SessionInputPendingLocks);
    CHECK((last_pending(&fixture.input)->locks & 1u) == 1u);

    ev_repeat(25, 600);
    CHECK(fixture.input.repeat_known == 1u);
    CHECK(fixture.input.repeat_rate == 25);
    CHECK(fixture.input.repeat_delay_ms == 600);
    ev_repeat(-1, 0);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostInvalidArgument);
    CHECK(fixture.input.repeat_rate == 25);
    fixture_end(&fixture);
}

static void test_surface_table(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitNever, NULL);

    for (size_t index = 0; index < MaxSurfaceEntries; index++) {
        CHECK(session_input_map_surface(&fixture.glue, surface_at(index), 0x2000u + index, 1u) == SessionInputWaylandOk);
    }
    CHECK(session_input_map_surface(&fixture.glue, surface_at(MaxSurfaceEntries), WindowA, 1u) ==
        SessionInputWaylandTableFull);
    /* Re-mapping a known surface never needs a free slot. */
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), 0x2000u, 1u) == SessionInputWaylandOk);
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(MaxSurfaceEntries)) == SessionInputWaylandOk);
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(0)) == SessionInputWaylandOk);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(MaxSurfaceEntries), WindowA, 1u) == SessionInputWaylandOk);
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(MaxSurfaceEntries)) == SessionInputWaylandOk);
    for (size_t index = 0; index < MaxSurfaceEntries; index++) {
        CHECK(session_input_unmap_surface(&fixture.glue, surface_at(index)) == SessionInputWaylandOk);
    }

    /* Unmapping an unknown surface is a no-op success. */
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(3)) == SessionInputWaylandOk);

    /* Remapping a focused surface to a new identity re-enters the new identity. */
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(1), WindowB, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    CHECK(fixture.input.focus_window == WindowA);
    size_t before = fixture.input.pending_count;
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    CHECK(fixture.input.pending_count == before);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 4u) == SessionInputWaylandOk);
    CHECK(fixture.input.focus_window == WindowA);
    CHECK(fixture.input.focus_generation == 4u);
    CHECK(fixture.input.focused == 1u);
    /* A non-focused surface changing identity does not touch focus. */
    CHECK(session_input_map_surface(&fixture.glue, surface_at(1), 0x3000u, 1u) == SessionInputWaylandOk);
    CHECK(fixture.input.focus_window == WindowA);

    /* Unmapping the focused surface delivers the focus leave first. */
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(0)) == SessionInputWaylandOk);
    CHECK(fixture.input.focused == 0u);
    CHECK(fixture.glue.focused_surface == NULL);

    /* After the handler closes, the focus delivery reports rejection; the entry is still removed. */
    ev_enter(surface_at(1));
    CHECK(fixture.input.focused == 1u);
    fixture.handler.on_closing(fixture.handler.context, HostControlSuccess, g_sender);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(1), WindowA, 9u) == SessionInputWaylandHandlerRejected);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostClosed);
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(1)) == SessionInputWaylandHandlerRejected);
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(1)) == SessionInputWaylandOk);
    session_input_wayland_destroy(&fixture.glue);
}

static void test_attach_detach(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitNever, NULL);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    CHECK(fixture.input.focused == 1u);
    CHECK(session_input_detach_keyboard(&fixture.glue) == SessionInputWaylandOk);
    CHECK(fixture.glue.keyboard == NULL);
    CHECK(fixture.input.focused == 0u);
    CHECK(session_input_detach_keyboard(&fixture.glue) == SessionInputWaylandNotAttached);
    CHECK(session_input_attach_keyboard(&fixture.glue, keyboard()) == SessionInputWaylandOk);
    CHECK(fixture.glue.keyboard == keyboard());
    fixture.handler.on_closing(fixture.handler.context, HostControlSuccess, g_sender);
    ev_enter(surface_at(0));
    CHECK(fixture.glue.focused_surface == surface_at(0));
    CHECK(session_input_detach_keyboard(&fixture.glue) == SessionInputWaylandHandlerRejected);
    CHECK(fixture.glue.focused_surface == NULL);
    CHECK(fixture.glue.keyboard == NULL);
    session_input_wayland_destroy(&fixture.glue);
}

static void test_inhibit_lifecycle(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitFocused, manager());
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(1), WindowB, 1u) == SessionInputWaylandOk);

    /* Focus enter creates the inhibitor for the mapped surface with the stored seat. */
    ev_enter(surface_at(0));
    CHECK(g_inhibit_calls == 1u);
    CHECK(g_last_inhibit_manager == (const void *)manager());
    CHECK(g_last_inhibit_surface == (const void *)surface_at(0));
    CHECK(g_last_inhibit_seat == (const void *)seat());
    CHECK(fixture.glue.inhibitor != NULL);
    CHECK(fixture.glue.inhibitor_window == WindowA);
    CHECK(fixture.glue.inhibitor_generation == 1u);
    CHECK(fixture.input.inhibit.phase == 1u);
    struct zwp_keyboard_shortcuts_inhibitor_v1 *first = fixture.glue.inhibitor;

    /* Active and inactive events reach the handler. */
    ev_inhibitor(first, 1);
    CHECK(fixture.input.inhibit.phase == 2u);
    CHECK(session_input_last_post_status(&fixture.glue) == SessionInputPostOk);
    ev_inhibitor(first, 0);
    CHECK(fixture.input.inhibit.phase == 1u);

    /* Leave destroys the inhibitor exactly once. */
    ev_leave(surface_at(0));
    CHECK(g_destroy_calls == 1u);
    CHECK(g_last_destroyed == (const void *)first);
    CHECK(fixture.glue.inhibitor == NULL);
    CHECK(fixture.input.inhibit.phase == 0u);

    /* A new enter creates a second object. A late event from the destroyed object is ignored. */
    ev_enter(surface_at(1));
    CHECK(g_inhibit_calls == 2u);
    CHECK(fixture.glue.inhibitor_window == WindowB);
    CHECK(fixture.input.inhibit.phase == 1u);
    ev_inhibitor(first, 1);
    CHECK(fixture.input.inhibit.phase == 1u);
    ev_inhibitor(first, 0);
    CHECK(fixture.input.inhibit.phase == 1u);

    /* Unmapping the focused surface releases its inhibitor through the focus leave. */
    CHECK(session_input_unmap_surface(&fixture.glue, surface_at(1)) == SessionInputWaylandOk);
    CHECK(g_destroy_calls == 2u);
    CHECK(fixture.glue.inhibitor == NULL);

    /* Destroy with a live inhibitor releases it; destroy with none releases nothing. */
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    CHECK(g_inhibit_calls == 3u);
    fixture.handler.on_closing(fixture.handler.context, HostControlSuccess, g_sender);
    CHECK(g_destroy_calls == 3u);
    session_input_wayland_destroy(&fixture.glue);
    CHECK(g_destroy_calls == 3u);
    CHECK(fixture.glue.inhibitor == NULL);
}

static void test_inhibit_without_manager_and_failures(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitFocused, NULL);
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    CHECK(g_inhibit_calls == 0u);
    CHECK(fixture.glue.inhibitor == NULL);
    ev_leave(surface_at(0));
    CHECK(g_destroy_calls == 0u);
    fixture_end(&fixture);

    /* Crafted actions exercise the guard branches directly through the inhibit callback. */
    fixture_begin(&fixture, WaddleKeyboardInhibitFocused, manager());
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    waddle_keyboard_inhibit_result_t result;
    memset(&result, 0, sizeof result);
    result.count = 1u;
    result.actions[0].kind = WaddleKeyboardInhibitActionCreate;
    result.actions[0].window = 0x9999u;
    result.actions[0].generation = 9u;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_inhibit_calls == 0u);

    /* Unknown action kind is ignored. */
    result.actions[0].kind = 0u;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_inhibit_calls == 0u);
    CHECK(g_destroy_calls == 0u);

    /* Destroy for an identity that holds no live inhibitor is ignored. */
    result.actions[0].kind = WaddleKeyboardInhibitActionDestroy;
    result.actions[0].window = WindowA;
    result.actions[0].generation = 1u;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_destroy_calls == 0u);

    /* A failed inhibit request leaves no record. */
    g_inhibit_fail = 1;
    result.actions[0].kind = WaddleKeyboardInhibitActionCreate;
    result.actions[0].window = WindowA;
    result.actions[0].generation = 1u;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_inhibit_calls == 1u);
    CHECK(fixture.glue.inhibitor == NULL);
    g_inhibit_fail = 0;

    /* A listener registration failure destroys the new object and keeps no record. */
    g_listener_fail = 1;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_inhibit_calls == 2u);
    CHECK(g_destroy_calls == 1u);
    CHECK(fixture.glue.inhibitor == NULL);
    g_listener_fail = 0;

    /* A Destroy naming another identity leaves a live inhibitor alone; so does the same window at
     * another generation. */
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(fixture.glue.inhibitor != NULL);
    result.actions[0].kind = WaddleKeyboardInhibitActionDestroy;
    result.actions[0].window = WindowB;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_destroy_calls == 1u);
    CHECK(fixture.glue.inhibitor != NULL);
    result.actions[0].window = WindowA;
    result.actions[0].generation = 2u;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_destroy_calls == 1u);
    CHECK(fixture.glue.inhibitor != NULL);
    result.actions[0].generation = 1u;

    /* A second Create while a record exists destroys the stale object first. */
    result.actions[0].kind = WaddleKeyboardInhibitActionCreate;
    result.actions[0].window = WindowA;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_destroy_calls == 2u);
    CHECK(g_inhibit_calls == 4u);
    CHECK(fixture.glue.inhibitor != NULL);

    /* A Destroy action that matches releases the live object. */
    result.actions[0].kind = WaddleKeyboardInhibitActionDestroy;
    session_input_wayland_apply_inhibit(&fixture.glue, &result);
    CHECK(g_destroy_calls == 3u);
    CHECK(fixture.glue.inhibitor == NULL);
    fixture_end(&fixture);
}

static void test_surface_state(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitFullscreen, manager());
    CHECK(session_input_map_surface(&fixture.glue, surface_at(0), WindowA, 1u) == SessionInputWaylandOk);
    ev_enter(surface_at(0));
    CHECK(g_inhibit_calls == 0u);

    /* Fullscreen on the focused window creates the inhibitor; leaving fullscreen destroys it. */
    CHECK(session_input_post_surface_state(&fixture.glue, WindowA, 1u, true, false) == SessionInputWaylandOk);
    CHECK(g_inhibit_calls == 1u);
    CHECK(fixture.glue.inhibitor != NULL);
    CHECK(session_input_post_surface_state(&fixture.glue, WindowA, 1u, false, false) == SessionInputWaylandOk);
    CHECK(g_destroy_calls == 1u);
    CHECK(fixture.glue.inhibitor == NULL);

    /* Pointer lock alone also requests the inhibitor. */
    CHECK(session_input_post_surface_state(&fixture.glue, WindowA, 1u, false, true) == SessionInputWaylandOk);
    CHECK(g_inhibit_calls == 2u);
    fixture.handler.on_closing(fixture.handler.context, HostControlSuccess, g_sender);
    session_input_wayland_destroy(&fixture.glue);
}

/**
 * @brief Randomized sequence over enter, leave, key, map, unmap, keymap, modifiers and surface state.
 *        The handler stays in Ready; the test checks that the glue never crashes and that, after every
 *        surface is unmapped, the handler holds no focus and the glue holds no inhibitor.
 */
static uint32_t g_rng_state = 0x9E3779B9u;

static uint32_t next_random(uint32_t bound) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return (g_rng_state >> 8) % bound;
}

static void test_random_sequence(void) {
    fixture_t fixture;
    fixture_begin(&fixture, WaddleKeyboardInhibitFullscreen, manager());
    for (size_t step = 0; step < RandomSteps; step++) {
        uint32_t index = next_random(RandomSurfaces);
        uint32_t window = 0x4000u + next_random(4u);
        uint64_t generation = 1u + next_random(3u);
        switch (next_random(9u)) {
        case 0:
            ev_enter(next_random(4u) == 0 ? NULL : surface_at(index));
            break;
        case 1:
            ev_leave(surface_at(index));
            break;
        case 2:
            ev_key(next_random(800u), next_random(3u));
            break;
        case 3:
            (void)session_input_map_surface(&fixture.glue, surface_at(index), window, generation);
            break;
        case 4:
            (void)session_input_unmap_surface(&fixture.glue, surface_at(index));
            break;
        case 5: {
            size_t size = 0;
            int fd = make_keymap_fd(next_random(2u) == 0 ? KeymapFixture : KeymapMalformed, &size);
            ev_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
            CHECK(fd_closed(fd));
            break;
        }
        case 6:
            ev_modifiers(next_random(8u), next_random(8u), next_random(8u));
            break;
        case 7:
            (void)session_input_post_surface_state(&fixture.glue, window, generation, next_random(2u) == 0,
                next_random(2u) == 0);
            break;
        default:
            if (step % 16u == 0u) fixture.handler.on_tick(fixture.handler.context, 0u, g_sender);
            break;
        }
        g_now_ns += 1000000ull;
    }
    for (size_t index = 0; index < RandomSurfaces; index++) {
        (void)session_input_unmap_surface(&fixture.glue, surface_at(index));
    }
    CHECK(fixture.input.focused == 0u);
    CHECK(fixture.glue.focused_surface == NULL);
    CHECK(fixture.glue.inhibitor == NULL);
    fixture_end(&fixture);
}

int main(void) {
    test_init_and_argument_errors();
    test_keymap_delivery();
    test_focus_and_keys();
    test_modifiers_and_repeat();
    test_surface_table();
    test_attach_detach();
    test_inhibit_lifecycle();
    test_inhibit_without_manager_and_failures();
    test_surface_state();
    test_random_sequence();
    printf("session_input_wayland_test: all checks passed\n");
    return 0;
}
