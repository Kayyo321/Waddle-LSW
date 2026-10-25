/**
 * @file window_state_adapter_suite.c
 * @brief Double-backed suites for the window-state Win32 adapter (src/interop/window_state_adapter_windows.c).
 *
 * Runs on every platform against tests/interop/window_state_adapter_double.h. Each test returns 0 on
 * success and 1 at the first failed Check. main() runs every test. Every adapter created here is
 * destroyed, so the sanitizer build reports no leak. The suite checks exact native call sequences,
 * every failure point of each action sequence, stale and reused windows, fullscreen rollback, threads,
 * re-entrancy, observation bits, capacity and a seeded random run with fault injection.
 */
#include "window_state_adapter_double.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "window_state_adapter_suite.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* Window identity and the default double window. */
#define HwndMain ((uintptr_t)0x1000)
#define HwndOther ((uintptr_t)0x2000)
#define IdMain ((uint64_t)1)
#define GenMain ((uint32_t)1)
#define OwnerThread ((uint32_t)100)
#define MainProcess ((uint32_t)7)
#define MainThread ((uint32_t)9)
#define ForeignThread ((uint32_t)200)

/** @brief Every member of window_state_native_api_t, for the NULL-member checks. */
#define WsaApiMembers(X) X(is_window) X(window_thread) X(is_iconic) X(is_zoomed) X(get_style) X(set_style) \
    X(show_window) X(set_window_pos) X(get_placement) X(set_placement) X(get_rect) X(monitor_rect) \
    X(foreground_window) X(set_foreground_window) X(set_active_window) X(attach_thread_input) \
    X(post_message) X(current_thread_id) X(last_error) X(extended_frame_bounds)

static waddle_window_state_action_t action_of(uint32_t kind);

/** @brief A double, its adapter, and the admitted main window. */
typedef struct fixture_t {
    wsa_double_t d;
    window_state_adapter_t *a;
    /** @brief HWND currently admitted under the main identity. */
    uintptr_t window;
} fixture_t;

static const window_state_rect_t MainRect = {100, 100, 500, 400};

static int fixture_open(fixture_t *f) {
    wsa_double_init(&f->d, OwnerThread);
    if (wsa_double_add(&f->d, HwndMain, MainProcess, MainThread, MainRect) == NULL) return 1;
    f->a = NULL;
    f->window = HwndMain;
    if (window_state_adapter_create(wsa_double_api(), &f->d, &f->a) != WindowStateAdapterOk) return 1;
    if (window_state_adapter_admit(f->a, HwndMain, IdMain, GenMain) != WindowStateAdapterOk) return 1;
    return 0;
}

/**
 * @brief Tears a fixture down. A window left fullscreen is first returned through LeaveFullscreen with
 *        faults disabled, because destroy refuses to free an adapter that still holds saved state.
 */
static void fixture_close(fixture_t *f) {
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    uint64_t completed = 0;
    if (f->a == NULL) return;
    if (window_state_adapter_destroy(&f->a) == WindowStateAdapterBusy) {
        wsa_double_disarm(&f->d);
        (void)window_state_adapter_execute(f->a, f->window, IdMain, GenMain, &leave, 1, &completed);
        (void)window_state_adapter_destroy(&f->a);
    }
}

/** @brief Adds the second window and makes it the foreground window under a foreground lock. */
static void prepare_locked_foreground(wsa_double_t *d, uint32_t other_thread) {
    window_state_rect_t rect = {0, 0, 200, 200};
    wsa_double_add(d, HwndOther, 8, other_thread, rect);
    d->foreground = HwndOther;
    d->foreground_locked = 1;
}

static waddle_window_state_action_t action_of(uint32_t kind) {
    waddle_window_state_action_t value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    return value;
}

static waddle_window_state_action_t geometry_of(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    waddle_window_state_action_t value = action_of(WaddleWindowStateActionGuestSetGeometry);
    value.x = x;
    value.y = y;
    value.width = width;
    value.height = height;
    return value;
}

static window_state_adapter_status_t run(fixture_t *f, const waddle_window_state_action_t *actions, uint64_t count,
                                         uint64_t *completed) {
    return window_state_adapter_execute(f->a, HwndMain, IdMain, GenMain, actions, count, completed);
}

static int log_matches(const wsa_double_t *d, const wsa_double_entry_t *expected, uint32_t count) {
    if (d->log_count != count) return 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (d->log[index] != expected[index]) return 0;
    }
    return 1;
}

static int log_is_prefix(const wsa_double_t *d, const wsa_double_entry_t *expected, uint32_t count) {
    if (d->log_count < count) return 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (d->log[index] != expected[index]) return 0;
    }
    return 1;
}

static uint32_t window_bits_of(const wsa_double_t *d, uintptr_t window) {
    const wsa_double_window_t *record = NULL;
    uint32_t bits = 0;
    for (uint32_t index = 0; index < WsaDoubleMaxWindows; ++index) {
        if (d->windows[index].window == window) record = &d->windows[index];
    }
    if (record == NULL) return 0;
    if (record->iconic != 0) bits |= WaddleWindowStateMinimized;
    if (record->zoomed != 0) bits |= WaddleWindowStateMaximized;
    return bits;
}

/* ---- Create, destroy, and the NULL-member contract ---------------------------------------------- */

static int test_create_and_destroy_contract(void) {
    wsa_double_t d;
    window_state_adapter_t *adapter = NULL;
    window_state_adapter_t *missing = NULL;
    window_state_adapter_t **slot = &adapter;
    window_state_native_api_t api_copy;

    wsa_double_init(&d, OwnerThread);
    Check(window_state_adapter_create(NULL, &d, &adapter) == WindowStateAdapterInvalidArgument);
    Check(adapter == NULL);
    Check(window_state_adapter_create(wsa_double_api(), &d, NULL) == WindowStateAdapterInvalidArgument);

#define NullMemberCheck(member) { \
        api_copy = *wsa_double_api(); \
        api_copy.member = NULL; \
        missing = (window_state_adapter_t *)&d; \
        Check(window_state_adapter_create(&api_copy, &d, &missing) == WindowStateAdapterInvalidArgument); \
        Check(missing == NULL); \
    }
    WsaApiMembers(NullMemberCheck)
#undef NullMemberCheck

    Check(window_state_adapter_create(wsa_double_api(), &d, &adapter) == WindowStateAdapterOk);
    Check(adapter != NULL);
    Check(window_state_adapter_last_error(adapter) == 0);
    Check(window_state_adapter_destroy(NULL) == WindowStateAdapterInvalidArgument);
    d.current_thread = ForeignThread;
    Check(window_state_adapter_destroy(slot) == WindowStateAdapterWrongThread);
    Check(adapter != NULL);
    d.current_thread = OwnerThread;
    Check(window_state_adapter_destroy(slot) == WindowStateAdapterOk);
    Check(adapter == NULL);
    Check(window_state_adapter_destroy(slot) == WindowStateAdapterOk);
    return 0;
}

static int test_destroy_is_busy_while_fullscreen(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    window_state_adapter_t *slot_holder;

    Check(fixture_open(&f) == 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
    slot_holder = f.a;
    Check(window_state_adapter_destroy(&slot_holder) == WindowStateAdapterBusy);
    Check(slot_holder != NULL);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterBusy);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterOk);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterOk);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterNotAdmitted);
    fixture_close(&f);
    return 0;
}

/* ---- Admission ---------------------------------------------------------------------------------- */

static int test_admission_rules(void) {
    fixture_t f;
    window_state_rect_t rect = {0, 0, 10, 10};
    uint32_t bits = 0;
    wsa_double_window_t *record;

    Check(fixture_open(&f) == 0);
    Check(window_state_adapter_admit(f.a, 0, IdMain, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_admit(f.a, HwndOther, 0, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_admit(f.a, HwndOther, IdMain, 0) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_admit(f.a, HwndMain, IdMain, GenMain) == WindowStateAdapterAlreadyAdmitted);
    Check(window_state_adapter_admit(f.a, HwndMain, 2, GenMain) == WindowStateAdapterAlreadyAdmitted);
    Check(window_state_adapter_admit(f.a, HwndOther, IdMain, GenMain) == WindowStateAdapterAlreadyAdmitted);
    Check(window_state_adapter_admit(f.a, 0x3000, 3, 1) == WindowStateAdapterNotFound);
    Check(window_state_adapter_observe(f.a, 0x3000, 3, 1, &bits) == WindowStateAdapterNotAdmitted);

    /* A failing thread query leaves nothing admitted. */
    wsa_double_add(&f.d, 0x3000, 5, 55, rect);
    wsa_double_arm(&f.d, WsaDoubleWindowThread, 1, 0);
    Check(window_state_adapter_admit(f.a, 0x3000, 3, 1) == WindowStateAdapterNativeFailure);
    Check(window_state_adapter_last_error(f.a) == f.d.error_value);
    Check(window_state_adapter_observe(f.a, 0x3000, 3, 1, &bits) == WindowStateAdapterNotAdmitted);
    wsa_double_disarm(&f.d);
    Check(window_state_adapter_admit(f.a, 0x3000, 3, 1) == WindowStateAdapterOk);
    Check(window_state_adapter_release(f.a, 3, 1) == WindowStateAdapterOk);

    /* A window whose thread id is zero is refused. */
    record = wsa_double_find(&f.d, 0x3000);
    record->thread_id = 0;
    Check(window_state_adapter_admit(f.a, 0x3000, 3, 1) == WindowStateAdapterNativeFailure);
    fixture_close(&f);
    return 0;
}

static int test_capacity_is_bounded_at_sixty_four(void) {
    fixture_t f;
    window_state_rect_t rect = {0, 0, 10, 10};
    Check(fixture_open(&f) == 0);
    for (uint32_t index = 1; index < WaddleWindowStateAdapterMaxWindows; ++index) {
        uintptr_t window = (uintptr_t)(0x10000u + index);
        wsa_double_add(&f.d, window, MainProcess, MainThread, rect);
        Check(window_state_adapter_admit(f.a, window, 100u + index, GenMain) == WindowStateAdapterOk);
    }
    wsa_double_add(&f.d, 0x20000, MainProcess, MainThread, rect);
    Check(window_state_adapter_admit(f.a, 0x20000, 999, GenMain) == WindowStateAdapterCapacityExhausted);
    Check(window_state_adapter_release(f.a, 101, GenMain) == WindowStateAdapterOk);
    Check(window_state_adapter_admit(f.a, 0x20000, 999, GenMain) == WindowStateAdapterOk);
    fixture_close(&f);
    return 0;
}

/* ---- Exact native call sequences per action ----------------------------------------------------- */

static int test_minimize_restore_maximize_sequences(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    waddle_window_state_action_t restore = action_of(WaddleWindowStateActionGuestRestore);
    waddle_window_state_action_t maximize = action_of(WaddleWindowStateActionGuestMaximize);
    const wsa_double_entry_t expected[] = {WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleShowWindow};
    uint32_t bits = 0;

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterOk);
    Check(completed == 1);
    Check(log_matches(&f.d, expected, 3));
    Check(f.d.windows[0].iconic == 1);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check(bits == (WaddleWindowStateMinimized | WaddleWindowStateResizable | WaddleWindowStateMaximizable |
                   WaddleWindowStateMinimizable));

    wsa_double_reset(&f.d);
    Check(run(&f, &restore, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, expected, 3));
    Check(f.d.windows[0].iconic == 0 && f.d.windows[0].zoomed == 0);

    wsa_double_reset(&f.d);
    Check(run(&f, &maximize, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, expected, 3));
    Check(f.d.windows[0].zoomed == 1);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateMaximized) != 0 && (bits & WaddleWindowStateMinimized) == 0);
    fixture_close(&f);
    return 0;
}

static int test_fullscreen_round_trip(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    const wsa_double_entry_t enter_expected[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleGetStyle, WsaDoubleGetRect,
        WsaDoubleGetPlacement, WsaDoubleMonitorRect, WsaDoubleSetStyle, WsaDoubleSetWindowPos};
    const wsa_double_entry_t leave_expected[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleSetStyle, WsaDoubleSetWindowPos, WsaDoubleIsIconic,
        WsaDoubleIsZoomed, WsaDoubleSetPlacement};
    uint32_t bits = 0;
    int64_t default_style;

    Check(fixture_open(&f) == 0);
    default_style = f.d.windows[0].style;
    wsa_double_reset(&f.d);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
    Check(completed == 1);
    Check(log_matches(&f.d, enter_expected, 8));
    Check((f.d.windows[0].style & (WsaFrameMaskCaption | WsaFrameMaskThickFrame)) == 0);
    Check(f.d.windows[0].rect.left == 0 && f.d.windows[0].rect.top == 0);
    Check(f.d.windows[0].rect.right == 1920 && f.d.windows[0].rect.bottom == 1080);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) != 0);
    Check((bits & WaddleWindowStateResizable) != 0);

    /* Entering again is refused before any native call. */
    wsa_double_reset(&f.d);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterAlreadyFullscreen);
    Check(completed == 0);
    Check(f.d.log_count == 0);

    wsa_double_reset(&f.d);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, leave_expected, 7));
    Check(f.d.windows[0].style == default_style);
    Check(f.d.windows[0].rect.left == MainRect.left && f.d.windows[0].rect.right == MainRect.right);
    Check(f.d.windows[0].rect.top == MainRect.top && f.d.windows[0].rect.bottom == MainRect.bottom);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) == 0);

    /* Leaving without a saved state is refused and invents nothing. */
    wsa_double_reset(&f.d);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterNoSavedState);
    Check(completed == 0);
    Check(f.d.log_count == 0);
    fixture_close(&f);
    return 0;
}

static int test_restore_does_not_leave_fullscreen(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t restore = action_of(WaddleWindowStateActionGuestRestore);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    uint32_t bits = 0;

    Check(fixture_open(&f) == 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
    Check(run(&f, &restore, 1, &completed) == WindowStateAdapterOk);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) != 0);
    Check((f.d.windows[0].style & WsaFrameMaskCaption) == 0);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterOk);
    fixture_close(&f);
    return 0;
}

/* SetWindowPos flags as Win32 defines them, for checking the flags the adapter passes. */
#define TestSwpNoZOrder 0x0004u
#define TestSwpNoActivate 0x0010u

static int test_geometry_and_close(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t geometry = geometry_of(10, 20, 320, 240);
    waddle_window_state_action_t zero_width = geometry_of(0, 0, 0, 240);
    waddle_window_state_action_t zero_height = geometry_of(0, 0, 240, 0);
    waddle_window_state_action_t too_wide = geometry_of(0, 0, WaddleWindowStateAdapterMaxExtent + 1, 240);
    waddle_window_state_action_t close = action_of(WaddleWindowStateActionGuestPostClose);
    const wsa_double_entry_t geometry_expected[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleGetRect, WsaDoubleExtendedFrameBounds, WsaDoubleSetWindowPos};
    const wsa_double_entry_t close_expected[] = {WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoublePostMessage};

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &zero_width, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(completed == 0 && f.d.log_count == 0);
    Check(run(&f, &zero_height, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(run(&f, &too_wide, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(f.d.log_count == 0);

    /* The origin x=10, y=20 is ignored (SWP_NOMOVE): the window keeps its position and takes the size. */
    Check(run(&f, &geometry, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, geometry_expected, 5));
    Check(f.d.windows[0].rect.left == MainRect.left && f.d.windows[0].rect.top == MainRect.top);
    Check(f.d.windows[0].rect.right == MainRect.left + 320 && f.d.windows[0].rect.bottom == MainRect.top + 240);

    wsa_double_reset(&f.d);
    Check(run(&f, &close, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, close_expected, 3));
    Check(f.d.windows[0].close_posts == 1);
    Check(f.d.windows[0].exists == 1);
    fixture_close(&f);
    return 0;
}

static int test_host_and_unknown_kinds_and_counts(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    waddle_window_state_action_t host = action_of(WaddleWindowStateActionHostSetMinimized);
    waddle_window_state_action_t zero = action_of(0);
    waddle_window_state_action_t unknown = action_of(99);
    waddle_window_state_action_t sequence[2];

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &host, 1, &completed) == WindowStateAdapterInvalidAction);
    Check(completed == 0 && f.d.log_count == 0);
    Check(run(&f, &zero, 1, &completed) == WindowStateAdapterInvalidAction);
    Check(run(&f, &unknown, 1, &completed) == WindowStateAdapterInvalidAction);
    wsa_double_reset(&f.d);

    sequence[0] = minimize;
    sequence[1] = host;
    wsa_double_reset(&f.d);
    Check(run(&f, sequence, 2, &completed) == WindowStateAdapterInvalidAction);
    Check(completed == 1);
    Check(f.d.windows[0].iconic == 1);

    Check(run(&f, NULL, 1, &completed) == WindowStateAdapterInvalidArgument);
    Check(run(&f, &minimize, 9, &completed) == WindowStateAdapterInvalidArgument);
    Check(completed == 0);
    Check(run(&f, NULL, 0, &completed) == WindowStateAdapterOk);
    Check(completed == 0);
    Check(window_state_adapter_execute(f.a, HwndMain, IdMain, GenMain, &minimize, 1, NULL) ==
          WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_execute(f.a, HwndOther, IdMain, GenMain, &minimize, 1, &completed) ==
          WindowStateAdapterInvalidArgument);
    Check(completed == 0);
    fixture_close(&f);
    return 0;
}

/**
 * @brief SetGeometry keeps the window origin (SWP_NOMOVE), applies the DWM frame adjustment of av_windows.c,
 *        and refuses an adjusted extent that is out of range or whose frame query fails.
 */
static int test_geometry_keeps_origin_and_adds_frame(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t configure = geometry_of(0, 0, 1024, 768);
    waddle_window_state_action_t moved = geometry_of(300, 200, 640, 480);
    waddle_window_state_action_t small = geometry_of(0, 0, 1, 1);
    waddle_window_state_action_t largest = geometry_of(0, 0, WaddleWindowStateAdapterMaxExtent, 10);
    int32_t visible_width;
    int32_t visible_height;

    /* Probe P2: a window at 100,100 with configure rect(0,0,1024,768) stays at 100,100 and becomes 1024 by 768. */
    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &configure, 1, &completed) == WindowStateAdapterOk);
    Check(completed == 1);
    Check(f.d.windows[0].rect.left == 100 && f.d.windows[0].rect.top == 100);
    Check(f.d.windows[0].rect.right - f.d.windows[0].rect.left == 1024);
    Check(f.d.windows[0].rect.bottom - f.d.windows[0].rect.top == 768);
    Check(f.d.set_window_pos_count == 1);
    Check((f.d.set_window_pos_flags[0] & WsaDoubleSwpNoMove) != 0);
    Check((f.d.set_window_pos_flags[0] & (TestSwpNoZOrder | TestSwpNoActivate)) == (TestSwpNoZOrder | TestSwpNoActivate));

    /* A nonzero origin from the caller is also ignored. */
    Check(run(&f, &moved, 1, &completed) == WindowStateAdapterOk);
    Check(f.d.windows[0].rect.left == 100 && f.d.windows[0].rect.top == 100);
    Check(f.d.windows[0].rect.right - f.d.windows[0].rect.left == 640);

    /* An invisible 8-pixel border per side: the outer rectangle grows by 16 and the visible frame is the request. */
    f.d.windows[0].frame_inset = 8;
    Check(run(&f, &configure, 1, &completed) == WindowStateAdapterOk);
    Check(f.d.windows[0].rect.right - f.d.windows[0].rect.left == 1024 + 16);
    Check(f.d.windows[0].rect.bottom - f.d.windows[0].rect.top == 768 + 16);
    visible_width = (f.d.windows[0].rect.right - 8) - (f.d.windows[0].rect.left + 8);
    visible_height = (f.d.windows[0].rect.bottom - 8) - (f.d.windows[0].rect.top + 8);
    Check(visible_width == 1024 && visible_height == 768);

    /* An adjusted extent of zero or below is refused (a negative border makes the visible frame larger). */
    f.d.windows[0].frame_inset = -1;
    wsa_double_reset(&f.d);
    Check(run(&f, &small, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(completed == 0);
    Check(f.d.set_window_pos_count == 0);

    /* An adjusted extent above the maximum is refused: 16384 plus the border of 2 pixels per side. */
    f.d.windows[0].frame_inset = 1;
    Check(run(&f, &largest, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(f.d.set_window_pos_count == 0);

    /* A failing frame query is a native failure with no window change. */
    f.d.windows[0].frame_inset = 0;
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleExtendedFrameBounds, 1, 0);
    Check(run(&f, &configure, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(completed == 0);
    Check(f.d.set_window_pos_count == 0);
    wsa_double_disarm(&f.d);
    fixture_close(&f);
    return 0;
}

/* ---- Activation and its fallback ---------------------------------------------------------------- */

static int test_activate_direct_and_fallback(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t activate = action_of(WaddleWindowStateActionGuestActivate);
    const wsa_double_entry_t direct[] = {WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleSetForegroundWindow};
    const wsa_double_entry_t fallback[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleSetForegroundWindow, WsaDoubleForegroundWindow,
        WsaDoubleWindowThread, WsaDoubleAttachThreadInput, WsaDoubleSetForegroundWindow, WsaDoubleSetActiveWindow,
        WsaDoubleAttachThreadInput};

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, direct, 3));
    Check(f.d.foreground == HwndMain);

    prepare_locked_foreground(&f.d, 77);
    f.d.foreground = HwndOther;
    wsa_double_reset(&f.d);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterOk);
    Check(log_matches(&f.d, fallback, 9));
    Check(f.d.foreground == HwndMain);
    Check(f.d.attached == 0);
    fixture_close(&f);
    return 0;
}

static int test_activate_failures(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t activate = action_of(WaddleWindowStateActionGuestActivate);
    const wsa_double_entry_t no_attach[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleSetForegroundWindow, WsaDoubleForegroundWindow,
        WsaDoubleWindowThread, WsaDoubleAttachThreadInput};
    const wsa_double_entry_t set_active_failed[] = {
        WsaDoubleIsWindow, WsaDoubleWindowThread, WsaDoubleSetForegroundWindow, WsaDoubleForegroundWindow,
        WsaDoubleWindowThread, WsaDoubleAttachThreadInput, WsaDoubleSetForegroundWindow, WsaDoubleSetActiveWindow,
        WsaDoubleAttachThreadInput};

    /* No foreground window at all. */
    Check(fixture_open(&f) == 0);
    f.d.foreground_locked = 1;
    f.d.foreground = 0;
    wsa_double_reset(&f.d);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(completed == 0);
    Check(f.d.log_count == 4);
    fixture_close(&f);

    /* Attach fails: nothing is detached because nothing was attached. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleAttachThreadInput, 1, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(log_is_prefix(&f.d, no_attach, 6) && f.d.log_count == 6);
    Check(f.d.attached == 0);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* Foreground thread is this thread: no attach, and the lock still refuses. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, OwnerThread);
    wsa_double_reset(&f.d);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.calls[WsaDoubleAttachThreadInput] == 0);
    fixture_close(&f);

    /* SetActiveWindow fails: the first failure is reported and the thread is still detached. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleSetActiveWindow, 1, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(log_matches(&f.d, set_active_failed, 9));
    Check(f.d.attached == 0);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* Detach fails: the failure is reported and the attachment remains in place. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleAttachThreadInput, 2, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.attached == 1);
    wsa_double_disarm(&f.d);
    fixture_close(&f);
    return 0;
}

/* ---- Stale, reused and foreign windows ---------------------------------------------------------- */

static int test_destroyed_window_is_stale_and_removed(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    uint32_t bits = 0;

    Check(fixture_open(&f) == 0);
    wsa_double_destroy_window(&f.d, HwndMain);
    wsa_double_reset(&f.d);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterStaleHandle);
    Check(completed == 0);
    Check(f.d.log_count == 1 && f.d.log[0] == WsaDoubleIsWindow);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_destroy(&f.a) == WindowStateAdapterOk);
    return 0;
}

static int test_reused_window_drops_saved_state_without_applying_it(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    const wsa_double_entry_t identity_only[] = {WsaDoubleIsWindow, WsaDoubleWindowThread};
    int64_t stripped;

    Check(fixture_open(&f) == 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
    stripped = f.d.windows[0].style;
    wsa_double_reuse(&f.d, HwndMain, 99, 9);
    wsa_double_reset(&f.d);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterStaleHandle);
    Check(log_matches(&f.d, identity_only, 2));
    Check(f.d.windows[0].style == stripped);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_destroy(&f.a) == WindowStateAdapterOk);
    return 0;
}

static int test_thread_change_is_stale_and_identity_failures(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    uint32_t bits = 0;

    Check(fixture_open(&f) == 0);
    wsa_double_reuse(&f.d, HwndMain, MainProcess, 10);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterStaleHandle);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_destroy(&f.a) == WindowStateAdapterOk);

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleIsWindow, 1, 0);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterStaleHandle);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* A failing thread query is not staleness: the entry survives and the next call works. */
    Check(fixture_open(&f) == 0);
    wsa_double_arm(&f.d, WsaDoubleWindowThread, 1, 0);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    wsa_double_disarm(&f.d);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterOk);
    fixture_close(&f);
    return 0;
}

/* ---- Fullscreen rollback and failure points ----------------------------------------------------- */

static int test_enter_fullscreen_rollback_paths(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    uint32_t bits = 0;
    int64_t original;

    /* SetStyle fails: no change, no record. */
    Check(fixture_open(&f) == 0);
    original = f.d.windows[0].style;
    wsa_double_arm(&f.d, WsaDoubleSetStyle, 1, 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.windows[0].style == original);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) == 0);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* SetWindowPos fails and the rollback succeeds: style restored, no record. */
    Check(fixture_open(&f) == 0);
    wsa_double_arm(&f.d, WsaDoubleSetWindowPos, 1, 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.windows[0].style == original);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) == 0);
    Check(window_state_adapter_last_error(f.a) == f.d.error_value);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* SetWindowPos fails and so does the rollback: the record is kept, and leaving undoes the style. */
    Check(fixture_open(&f) == 0);
    wsa_double_arm(&f.d, WsaDoubleSetWindowPos, 1, 0);
    wsa_double_arm(&f.d, WsaDoubleSetStyle, 2, 0);
    Check(run(&f, &enter, 1, &completed) == WindowStateAdapterNativeFailure);
    Check((f.d.windows[0].style & WsaFrameMaskCaption) == 0);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateFullscreen) != 0);
    wsa_double_disarm(&f.d);
    Check(run(&f, &leave, 1, &completed) == WindowStateAdapterOk);
    Check(f.d.windows[0].style == original);
    fixture_close(&f);
    return 0;
}

static int test_leave_fullscreen_keeps_record_until_all_calls_succeed(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    uint32_t bits = 0;
    wsa_double_entry_t stages[] = {WsaDoubleSetPlacement, WsaDoubleSetWindowPos};
    int64_t original;

    for (uint32_t stage = 0; stage < 2; ++stage) {
        Check(fixture_open(&f) == 0);
        original = f.d.windows[0].style;
        Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
        wsa_double_arm(&f.d, stages[stage], 1, 0);
        Check(run(&f, &leave, 1, &completed) == WindowStateAdapterNativeFailure);
        Check(completed == 0);
        Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
        Check((bits & WaddleWindowStateFullscreen) != 0);
        wsa_double_disarm(&f.d);
        Check(run(&f, &leave, 1, &completed) == WindowStateAdapterOk);
        Check(f.d.windows[0].style == original);
        fixture_close(&f);
    }
    return 0;
}

/**
 * @brief Fails each call of each entry that a sequence makes, one at a time, and checks the result.
 *
 * For every failure point: the call that fails is the one counted, the completed count is the number
 * of actions before the first non-completing action, and the observed Minimized and Maximized bits
 * equal the double's flags while the Fullscreen bit equals the existence of a saved record, which is
 * the one case where the double's style can be stripped without a record (a rollback that also failed).
 */
static int sweep_sequence(int prepare_fullscreen, int prepare_fallback, const waddle_window_state_action_t *actions,
                          uint64_t count) {
    fixture_t reference;
    uint32_t calls[WsaDoubleEntryCount];
    uint64_t completed = 0;

    /* Reference run without faults, to count the calls of each entry. */
    Check(fixture_open(&reference) == 0);
    if (prepare_fallback) prepare_locked_foreground(&reference.d, 77);
    if (prepare_fullscreen) {
        waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
        Check(run(&reference, &enter, 1, &completed) == WindowStateAdapterOk);
    }
    wsa_double_reset(&reference.d);
    Check(run(&reference, actions, count, &completed) != WindowStateAdapterInvalidArgument);
    memcpy(calls, reference.d.calls, sizeof(calls));
    fixture_close(&reference);

    for (uint32_t entry = 0; entry < WsaDoubleEntryCount; ++entry) {
        for (uint32_t nth = 1; nth <= calls[entry]; ++nth) {
            fixture_t f;
            window_state_adapter_status_t status;
            uint32_t bits = 0;
            Check(fixture_open(&f) == 0);
            if (prepare_fallback) prepare_locked_foreground(&f.d, 77);
            if (prepare_fullscreen) {
                waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
                Check(run(&f, &enter, 1, &completed) == WindowStateAdapterOk);
            }
            wsa_double_reset(&f.d);
            wsa_double_arm(&f.d, (wsa_double_entry_t)entry, nth, 0);
            completed = 99;
            status = run(&f, actions, count, &completed);
            wsa_double_disarm(&f.d);
            if (status == WindowStateAdapterOk) {
                Check(completed == count);
            } else {
                Check(completed < count);
                Check(status == WindowStateAdapterNativeFailure || status == WindowStateAdapterStaleHandle);
            }
            if (status != WindowStateAdapterStaleHandle) {
                Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
                Check((bits & WaddleWindowStateMinimized) == (window_bits_of(&f.d, HwndMain) & WaddleWindowStateMinimized));
                Check((bits & WaddleWindowStateMaximized) == (window_bits_of(&f.d, HwndMain) & WaddleWindowStateMaximized));
                /* Without a saved record the window must still have its caption. */
                if ((bits & WaddleWindowStateFullscreen) == 0) {
                    Check((wsa_double_record(&f.d, HwndMain)->style & WsaFrameMaskCaption) != 0);
                }
                fixture_close(&f);
            } else {
                Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
                Check(window_state_adapter_destroy(&f.a) == WindowStateAdapterOk);
            }
        }
    }
    return 0;
}

static int test_failure_points_of_every_sequence(void) {
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    waddle_window_state_action_t restore = action_of(WaddleWindowStateActionGuestRestore);
    waddle_window_state_action_t maximize = action_of(WaddleWindowStateActionGuestMaximize);
    waddle_window_state_action_t enter = action_of(WaddleWindowStateActionGuestEnterFullscreen);
    waddle_window_state_action_t leave = action_of(WaddleWindowStateActionGuestLeaveFullscreen);
    waddle_window_state_action_t activate = action_of(WaddleWindowStateActionGuestActivate);
    waddle_window_state_action_t close = action_of(WaddleWindowStateActionGuestPostClose);
    waddle_window_state_action_t geometry = geometry_of(10, 20, 320, 240);
    waddle_window_state_action_t mixed[] = {minimize, restore, maximize, geometry, close};
    waddle_window_state_action_t fullscreen_cycle[] = {enter, activate, leave, restore, maximize};
    waddle_window_state_action_t from_fullscreen[] = {leave, maximize, geometry};

    Check(sweep_sequence(0, 0, mixed, 5) == 0);
    Check(sweep_sequence(0, 0, fullscreen_cycle, 5) == 0);
    Check(sweep_sequence(1, 0, from_fullscreen, 3) == 0);
    Check(sweep_sequence(0, 1, &activate, 1) == 0);
    return 0;
}

/* ---- Observation bits and last error ------------------------------------------------------------ */

static int test_observation_bits(void) {
    fixture_t f;
    uint32_t bits = 0xdeadu;
    uint32_t sentinel = 0xdeadu;
    wsa_double_window_t *record;
    int64_t style_without_frame;

    Check(fixture_open(&f) == 0);
    record = &f.d.windows[0];
    record->style = 0;
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check(bits == 0);

    record->style = WsaFrameMaskThickFrame | WsaFrameMaskMinimizeBox;
    record->iconic = 1;
    record->zoomed = 1;
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check(bits == (WaddleWindowStateMinimized | WaddleWindowStateMaximized | WaddleWindowStateResizable |
                   WaddleWindowStateMinimizable));

    style_without_frame = WsaFrameMaskMaximizeBox;
    record->style = style_without_frame;
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateMaximizable) != 0 && (bits & WaddleWindowStateResizable) == 0);

    /* A failed observation leaves the caller's output untouched. */
    bits = sentinel;
    wsa_double_arm(&f.d, WsaDoubleGetStyle, 1, 0);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNativeFailure);
    Check(bits == sentinel);
    wsa_double_disarm(&f.d);
    fixture_close(&f);
    return 0;
}

static int test_last_error_and_foreign_thread(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    uint32_t bits = 0;

    Check(fixture_open(&f) == 0);
    f.d.error_value = 87u;
    wsa_double_arm(&f.d, WsaDoubleShowWindow, 1, 0);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(window_state_adapter_last_error(f.a) == 87u);

    f.d.current_thread = ForeignThread;
    wsa_double_reset(&f.d);
    Check(window_state_adapter_last_error(f.a) == 0);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterWrongThread);
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterWrongThread);
    Check(completed == 0);
    Check(window_state_adapter_admit(f.a, HwndOther, 9, 9) == WindowStateAdapterWrongThread);
    Check(window_state_adapter_release(f.a, IdMain, GenMain) == WindowStateAdapterWrongThread);
    Check(f.d.log_count == 0);
    f.d.current_thread = OwnerThread;
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    fixture_close(&f);
    return 0;
}

/* ---- Re-entrancy ---------------------------------------------------------------------------------- */

static window_state_adapter_t *reentry_adapter;
static window_state_adapter_t **reentry_slot;
static window_state_adapter_status_t reentry_observe_status;
static window_state_adapter_status_t reentry_destroy_status;
static window_state_adapter_status_t reentry_execute_status;

/** @brief ShowWindow that calls back into the same adapter before it acts, as a dispatched message would. */
static int reentrant_show_window(void *context, uintptr_t window, int32_t command) {
    uint32_t bits = 0;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    reentry_observe_status = window_state_adapter_observe(reentry_adapter, window, IdMain, GenMain, &bits);
    reentry_destroy_status = window_state_adapter_destroy(reentry_slot);
    reentry_execute_status = window_state_adapter_execute(reentry_adapter, window, IdMain, GenMain, &minimize, 1,
                                                          &completed);
    return wsa_double_show_window(context, window, command);
}

static int test_reentry_is_refused(void) {
    wsa_double_t d;
    window_state_native_api_t api_copy;
    window_state_adapter_t *adapter = NULL;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    uint32_t bits = 0;

    wsa_double_init(&d, OwnerThread);
    Check(wsa_double_add(&d, HwndMain, MainProcess, MainThread, MainRect) != NULL);
    api_copy = *wsa_double_api();
    api_copy.show_window = reentrant_show_window;
    Check(window_state_adapter_create(&api_copy, &d, &adapter) == WindowStateAdapterOk);
    Check(window_state_adapter_admit(adapter, HwndMain, IdMain, GenMain) == WindowStateAdapterOk);
    reentry_adapter = adapter;
    reentry_slot = &adapter;
    Check(window_state_adapter_execute(adapter, HwndMain, IdMain, GenMain, &minimize, 1, &completed) ==
          WindowStateAdapterOk);
    Check(completed == 1);
    Check(reentry_observe_status == WindowStateAdapterReentrant);
    Check(reentry_destroy_status == WindowStateAdapterReentrant);
    Check(reentry_execute_status == WindowStateAdapterReentrant);
    Check(adapter != NULL);
    Check(window_state_adapter_observe(adapter, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterOk);
    Check((bits & WaddleWindowStateMinimized) != 0);
    Check(window_state_adapter_destroy(&adapter) == WindowStateAdapterOk);
    return 0;
}

/* ---- Seeded random sequences with fault injection ----------------------------------------------- */

static uint64_t random_state = UINT64_C(0x2545f4914f6cdd1d);

static uint32_t random_pick(uint32_t bound) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (uint32_t)(random_state % bound);
}

static int test_random_sequences_with_faults(void) {
    static const uint32_t Kinds[] = {
        WaddleWindowStateActionGuestMinimize, WaddleWindowStateActionGuestRestore,
        WaddleWindowStateActionGuestMaximize, WaddleWindowStateActionGuestEnterFullscreen,
        WaddleWindowStateActionGuestLeaveFullscreen, WaddleWindowStateActionGuestActivate,
        WaddleWindowStateActionGuestSetGeometry, WaddleWindowStateActionGuestPostClose,
        WaddleWindowStateActionHostSetMinimized};
    const uint32_t KindCount = (uint32_t)(sizeof(Kinds) / sizeof(Kinds[0]));
    const window_state_rect_t rect = {0, 0, 300, 300};
    uint64_t status_counts[16];
    uint64_t ok_actions = 0;
    uint32_t next_window = 0;

    memset(status_counts, 0, sizeof(status_counts));
    for (uint32_t round = 0; round < 24; ++round) {
        fixture_t f;
        uintptr_t window = HwndMain;
        Check(fixture_open(&f) == 0);
        f.d.rng = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)(round + 1u);
        for (uint32_t step = 0; step < 60; ++step) {
            waddle_window_state_action_t batch[3];
            uint64_t count = 1u + random_pick(3u);
            uint64_t completed = 0;
            window_state_adapter_status_t status;
            uint32_t bits = 0;
            int destroyed = 0;

            for (uint64_t index = 0; index < count; ++index) {
                uint32_t kind = Kinds[random_pick(KindCount)];
                if (kind == WaddleWindowStateActionGuestSetGeometry) {
                    batch[index] = geometry_of((int32_t)random_pick(200u), (int32_t)random_pick(200u),
                                               random_pick(4u) == 0u ? 0u : 1u + random_pick(400u),
                                               random_pick(4u) == 0u ? 0u : 1u + random_pick(300u));
                } else {
                    batch[index] = action_of(kind);
                }
            }

            f.d.fault_per_mille = 120;
            status = window_state_adapter_execute(f.a, window, IdMain, GenMain, batch, count, &completed);
            f.d.fault_per_mille = 0;
            status_counts[status] += 1u;
            if (status == WindowStateAdapterOk) {
                Check(completed == count);
                ok_actions += count;
            } else {
                Check(completed < count);
            }

            if (random_pick(25u) == 0u) {
                wsa_double_destroy_window(&f.d, window);
                destroyed = 1;
            }
            if (status == WindowStateAdapterStaleHandle || destroyed != 0) {
                /* A destroyed window must be reported stale by the next call, which also removes the entry. */
                waddle_window_state_action_t probe = action_of(WaddleWindowStateActionGuestPostClose);
                if (destroyed != 0 && status != WindowStateAdapterStaleHandle) {
                    Check(window_state_adapter_execute(f.a, window, IdMain, GenMain, &probe, 1, &completed) ==
                          WindowStateAdapterStaleHandle);
                    Check(completed == 0);
                }
                Check(window_state_adapter_observe(f.a, window, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
                next_window += 1u;
                window = (uintptr_t)(0x30000u + next_window * 16u);
                f.window = window;
                Check(wsa_double_add(&f.d, window, MainProcess, MainThread, rect) != NULL);
                Check(window_state_adapter_admit(f.a, window, IdMain, GenMain) == WindowStateAdapterOk);
            }

            /* Invariants are read with faults disabled. */
            Check(window_state_adapter_observe(f.a, window, IdMain, GenMain, &bits) == WindowStateAdapterOk);
            Check((bits & WaddleWindowStateMinimized) == (window_bits_of(&f.d, window) & WaddleWindowStateMinimized));
            Check((bits & WaddleWindowStateMaximized) == (window_bits_of(&f.d, window) & WaddleWindowStateMaximized));
            if ((bits & WaddleWindowStateFullscreen) == 0) {
                Check((wsa_double_record(&f.d, window)->style & WsaFrameMaskCaption) != 0);
            }
        }
        fixture_close(&f);
    }
    Check(status_counts[WindowStateAdapterOk] > 50);
    Check(status_counts[WindowStateAdapterNativeFailure] > 0);
    Check(status_counts[WindowStateAdapterStaleHandle] > 0);
    Check(ok_actions > 100);
    return 0;
}

/* ---- Argument, identity and failure forms not reached above ------------------------------------- */

/* calloc is wrapped (link flag --wrap=calloc) so the out-of-memory path of create can be taken. */
extern void *__real_calloc(size_t count, size_t size);
static int fail_next_calloc = 0;
void *__wrap_calloc(size_t count, size_t size) {
    if (fail_next_calloc != 0) {
        fail_next_calloc = 0;
        return NULL;
    }
    return __real_calloc(count, size);
}

static int test_create_out_of_memory(void) {
    wsa_double_t d;
    window_state_adapter_t *adapter = (window_state_adapter_t *)&d;
    wsa_double_init(&d, OwnerThread);
    fail_next_calloc = 1;
    Check(window_state_adapter_create(wsa_double_api(), &d, &adapter) == WindowStateAdapterOutOfMemory);
    Check(adapter == NULL);
    fail_next_calloc = 0;
    return 0;
}

static int test_native_api_is_null_off_windows(void) {
#ifndef _WIN32
    Check(window_state_native_api() == NULL);
#endif
    return 0;
}

static int test_zero_and_null_argument_forms(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    uint32_t bits = 0;
    window_state_adapter_t *none = NULL;

    Check(fixture_open(&f) == 0);
    /* Each zero identity component on its own. */
    Check(window_state_adapter_admit(f.a, HwndOther, 0, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_admit(f.a, HwndOther, IdMain, 0) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_release(f.a, 0, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_release(f.a, IdMain, 0) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_observe(f.a, HwndMain, 0, GenMain, &bits) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, 0, &bits) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, NULL) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_execute(f.a, HwndMain, 0, GenMain, &minimize, 1, &completed) ==
          WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_execute(f.a, HwndMain, IdMain, 0, &minimize, 1, &completed) ==
          WindowStateAdapterInvalidArgument);
    /* A NULL adapter, in every entry that takes one. */
    Check(window_state_adapter_admit(none, HwndOther, IdMain, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_release(none, IdMain, GenMain) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_observe(none, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_execute(none, HwndMain, IdMain, GenMain, &minimize, 1, &completed) ==
          WindowStateAdapterInvalidArgument);
    Check(window_state_adapter_last_error(none) == 0);
    /* Each identity component differing from the admitted pair is NotAdmitted, not a match. */
    Check(window_state_adapter_observe(f.a, HwndMain, 2, GenMain, &bits) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, 2, &bits) == WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_execute(f.a, HwndMain, 2, GenMain, &minimize, 1, &completed) ==
          WindowStateAdapterNotAdmitted);
    Check(window_state_adapter_execute(f.a, HwndMain, IdMain, 2, &minimize, 1, &completed) ==
          WindowStateAdapterNotAdmitted);
    Check(completed == 0);
    fixture_close(&f);
    return 0;
}

static int test_observe_identity_and_query_failures(void) {
    fixture_t f;
    uint32_t bits = 0xbeefu;

    Check(fixture_open(&f) == 0);
    Check(window_state_adapter_observe(f.a, HwndOther, IdMain, GenMain, &bits) == WindowStateAdapterInvalidArgument);
    Check(bits == 0xbeefu);

    wsa_double_arm(&f.d, WsaDoubleIsIconic, 1, 0);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNativeFailure);
    Check(bits == 0xbeefu);
    wsa_double_disarm(&f.d);

    wsa_double_arm(&f.d, WsaDoubleIsZoomed, 1, 0);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNativeFailure);
    Check(bits == 0xbeefu);
    wsa_double_disarm(&f.d);

    /* A destroyed window is stale when observed, and the entry is removed. */
    wsa_double_destroy_window(&f.d, HwndMain);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterStaleHandle);
    Check(window_state_adapter_observe(f.a, HwndMain, IdMain, GenMain, &bits) == WindowStateAdapterNotAdmitted);
    fixture_close(&f);
    return 0;
}

static int test_thread_zero_at_identity_and_foreground(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t minimize = action_of(WaddleWindowStateActionGuestMinimize);
    waddle_window_state_action_t activate = action_of(WaddleWindowStateActionGuestActivate);
    wsa_double_window_t *record;

    /* The identity query reports thread 0 after admission: a native failure, not a match. */
    Check(fixture_open(&f) == 0);
    record = wsa_double_find(&f.d, HwndMain);
    record->thread_id = 0;
    Check(run(&f, &minimize, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(completed == 0);
    record->thread_id = MainThread;
    fixture_close(&f);

    /* The foreground window's thread query fails during the fallback. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleWindowThread, 2, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* The foreground window reports thread 0: refused. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_find(&f.d, HwndOther)->thread_id = 0;
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    fixture_close(&f);
    return 0;
}

static int test_fallback_first_failure_is_kept(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t activate = action_of(WaddleWindowStateActionGuestActivate);

    /* The retry of SetForegroundWindow fails, and so does SetActiveWindow: the first failure stands and
     * the attachment is still detached. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleSetForegroundWindow, 2, 0);
    wsa_double_arm(&f.d, WsaDoubleSetActiveWindow, 1, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.attached == 0);
    wsa_double_disarm(&f.d);
    fixture_close(&f);

    /* The retry of SetForegroundWindow fails, and the detach also fails: detach is still attempted. */
    Check(fixture_open(&f) == 0);
    prepare_locked_foreground(&f.d, 77);
    wsa_double_reset(&f.d);
    wsa_double_arm(&f.d, WsaDoubleSetForegroundWindow, 2, 0);
    wsa_double_arm(&f.d, WsaDoubleAttachThreadInput, 2, 0);
    Check(run(&f, &activate, 1, &completed) == WindowStateAdapterNativeFailure);
    Check(f.d.calls[WsaDoubleAttachThreadInput] == 2);
    wsa_double_disarm(&f.d);
    fixture_close(&f);
    return 0;
}

static int test_geometry_height_above_maximum(void) {
    fixture_t f;
    uint64_t completed = 0;
    waddle_window_state_action_t tall = geometry_of(0, 0, 240, WaddleWindowStateAdapterMaxExtent + 1);
    waddle_window_state_action_t largest = geometry_of(0, 0, WaddleWindowStateAdapterMaxExtent, WaddleWindowStateAdapterMaxExtent);

    Check(fixture_open(&f) == 0);
    wsa_double_reset(&f.d);
    Check(run(&f, &tall, 1, &completed) == WindowStateAdapterInvalidGeometry);
    Check(f.d.log_count == 0);
    Check(run(&f, &largest, 1, &completed) == WindowStateAdapterOk);
    fixture_close(&f);
    return 0;
}

static int run_test(const char *name, int (*test)(void)) {
    int result = test();
    if (result != 0) {
        fprintf(stderr, "FAILED: %s\n", name);
    } else {
        printf("ok: %s\n", name);
    }
    return result;
}

int main(void) {
    int failures = 0;
    failures += run_test("create, destroy and NULL members", test_create_and_destroy_contract);
    failures += run_test("destroy and release are busy while fullscreen", test_destroy_is_busy_while_fullscreen);
    failures += run_test("admission rules", test_admission_rules);
    failures += run_test("capacity is bounded at sixty-four", test_capacity_is_bounded_at_sixty_four);
    failures += run_test("minimize, restore and maximize sequences", test_minimize_restore_maximize_sequences);
    failures += run_test("fullscreen round trip", test_fullscreen_round_trip);
    failures += run_test("restore does not leave fullscreen", test_restore_does_not_leave_fullscreen);
    failures += run_test("geometry and close", test_geometry_and_close);
    failures += run_test("geometry keeps origin and adds frame", test_geometry_keeps_origin_and_adds_frame);
    failures += run_test("host, unknown and count rules", test_host_and_unknown_kinds_and_counts);
    failures += run_test("activate direct and fallback", test_activate_direct_and_fallback);
    failures += run_test("activate failures", test_activate_failures);
    failures += run_test("destroyed window is stale and removed", test_destroyed_window_is_stale_and_removed);
    failures += run_test("reused window drops saved state unapplied", test_reused_window_drops_saved_state_without_applying_it);
    failures += run_test("thread change and identity failures", test_thread_change_is_stale_and_identity_failures);
    failures += run_test("enter fullscreen rollback paths", test_enter_fullscreen_rollback_paths);
    failures += run_test("leave keeps record until every call succeeds", test_leave_fullscreen_keeps_record_until_all_calls_succeed);
    failures += run_test("failure points of every sequence", test_failure_points_of_every_sequence);
    failures += run_test("observation bits", test_observation_bits);
    failures += run_test("last error and foreign thread", test_last_error_and_foreign_thread);
    failures += run_test("re-entry is refused", test_reentry_is_refused);
    failures += run_test("random sequences with faults", test_random_sequences_with_faults);
    failures += run_test("create out of memory", test_create_out_of_memory);
    failures += run_test("native table is NULL off Windows", test_native_api_is_null_off_windows);
    failures += run_test("zero and NULL argument forms", test_zero_and_null_argument_forms);
    failures += run_test("observe identity and query failures", test_observe_identity_and_query_failures);
    failures += run_test("zero thread at identity and foreground", test_thread_zero_at_identity_and_foreground);
    failures += run_test("first fallback failure is kept", test_fallback_first_failure_is_kept);
    failures += run_test("geometry height above maximum", test_geometry_height_above_maximum);
    if (failures != 0) {
        fprintf(stderr, "window_state_adapter_suite: %d test(s) failed\n", failures);
        return 1;
    }
    printf("window_state_adapter_suite: all tests passed\n");
    return 0;
}
