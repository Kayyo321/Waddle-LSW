/** @file keyboard_inhibit_abi.c @brief Independent C consumer for the shortcut-inhibit C ABI. */
#include "keyboard_inhibit.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(waddle_keyboard_inhibit_entry_t) == 16, "entry layout");
_Static_assert(sizeof(waddle_keyboard_inhibit_state_t) == 280, "state layout");
_Static_assert(sizeof(waddle_keyboard_inhibit_event_t) == 24, "event layout");
_Static_assert(sizeof(waddle_keyboard_inhibit_action_t) == 24, "action layout");
_Static_assert(sizeof(waddle_keyboard_inhibit_result_t) == 64, "result layout");

static waddle_keyboard_inhibit_event_t make_event(uint8_t kind, uint64_t window, uint32_t generation) {
    waddle_keyboard_inhibit_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.window = window;
    event.generation = generation;
    return event;
}

static waddle_keyboard_inhibit_event_t make_surface(uint64_t window, uint32_t generation, uint8_t fullscreen, uint8_t pointer_lock) {
    waddle_keyboard_inhibit_event_t event = make_event(WaddleKeyboardInhibitEventSurfaceState, window, generation);
    event.fullscreen = fullscreen;
    event.pointer_lock = pointer_lock;
    return event;
}

static void test_null_and_errors(void) {
    waddle_keyboard_inhibit_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_inhibit_result_t result;
    memset(&result, 0x3c, sizeof(result));
    waddle_keyboard_inhibit_event_t event = make_event(WaddleKeyboardInhibitEventFocusEnter, 1, 1);

    assert(waddle_keyboard_inhibit_init(NULL) == WaddleKeyboardInhibitNullArgument);
    assert(waddle_keyboard_inhibit_apply(NULL, &event, &result) == WaddleKeyboardInhibitNullArgument);
    assert(waddle_keyboard_inhibit_apply(&state, NULL, &result) == WaddleKeyboardInhibitNullArgument);
    assert(waddle_keyboard_inhibit_apply(&state, &event, NULL) == WaddleKeyboardInhibitNullArgument);

    waddle_keyboard_inhibit_event_t bad_kind = make_event(200, 1, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &bad_kind, &result) == WaddleKeyboardInhibitInvalidKind);
    waddle_keyboard_inhibit_event_t bad_mode = make_event(WaddleKeyboardInhibitEventConfigure, 0, 0);
    bad_mode.mode = 9;
    assert(waddle_keyboard_inhibit_apply(&state, &bad_mode, &result) == WaddleKeyboardInhibitInvalidMode);
    waddle_keyboard_inhibit_event_t bad_flag = make_surface(1, 1, 2, 0);
    assert(waddle_keyboard_inhibit_apply(&state, &bad_flag, &result) == WaddleKeyboardInhibitInvalidFlag);
    bad_flag = make_surface(1, 1, 0, 0xFF);
    assert(waddle_keyboard_inhibit_apply(&state, &bad_flag, &result) == WaddleKeyboardInhibitInvalidFlag);
    waddle_keyboard_inhibit_event_t zero_id = make_event(WaddleKeyboardInhibitEventFocusEnter, 0, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &zero_id, &result) == WaddleKeyboardInhibitInvalidIdentity);
    assert(result.count == 0x3c);
    assert(state.focused == 0 && state.config == WaddleKeyboardInhibitFullscreen);

    state.config = 3;
    assert(waddle_keyboard_inhibit_apply(&state, &event, &result) == WaddleKeyboardInhibitInvalidState);
    assert(waddle_keyboard_inhibit_init(&state) == WaddleKeyboardInhibitOk);
    assert(state.config == WaddleKeyboardInhibitFullscreen && state.focused == 0);
}

static void test_default_policy_and_flags(void) {
    waddle_keyboard_inhibit_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_inhibit_result_t result;

    waddle_keyboard_inhibit_event_t fullscreen = make_surface(3, 1, 1, 0);
    assert(waddle_keyboard_inhibit_apply(&state, &fullscreen, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 0);
    waddle_keyboard_inhibit_event_t enter = make_event(WaddleKeyboardInhibitEventFocusEnter, 3, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &enter, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 1 && result.actions[0].kind == WaddleKeyboardInhibitActionCreate);
    assert(result.actions[0].window == 3 && result.actions[0].generation == 1);

    waddle_keyboard_inhibit_event_t locked = make_surface(4, 1, 0, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &locked, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 0);
    waddle_keyboard_inhibit_event_t leave = make_event(WaddleKeyboardInhibitEventFocusLeave, 3, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &leave, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 1 && result.actions[0].kind == WaddleKeyboardInhibitActionDestroy);
    waddle_keyboard_inhibit_event_t enter_locked = make_event(WaddleKeyboardInhibitEventFocusEnter, 4, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &enter_locked, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 1 && result.actions[0].kind == WaddleKeyboardInhibitActionCreate);
}

static void test_session(void) {
    waddle_keyboard_inhibit_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_inhibit_result_t result;

    waddle_keyboard_inhibit_event_t configure = make_event(WaddleKeyboardInhibitEventConfigure, 0, 0);
    configure.mode = WaddleKeyboardInhibitFocused;
    assert(waddle_keyboard_inhibit_apply(&state, &configure, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 0 && !result.forward_shortcuts);

    waddle_keyboard_inhibit_event_t enter = make_event(WaddleKeyboardInhibitEventFocusEnter, 7, 2);
    assert(waddle_keyboard_inhibit_apply(&state, &enter, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 1 && result.actions[0].kind == WaddleKeyboardInhibitActionCreate);
    assert(!result.forward_shortcuts);

    waddle_keyboard_inhibit_event_t active = make_event(WaddleKeyboardInhibitEventInhibitorActive, 7, 2);
    assert(waddle_keyboard_inhibit_apply(&state, &active, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 0 && result.forward_shortcuts);

    waddle_keyboard_inhibit_event_t moved = make_event(WaddleKeyboardInhibitEventFocusEnter, 8, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &moved, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 2);
    assert(result.actions[0].kind == WaddleKeyboardInhibitActionDestroy && result.actions[0].window == 7);
    assert(result.actions[1].kind == WaddleKeyboardInhibitActionCreate && result.actions[1].window == 8);
    assert(!result.forward_shortcuts);

    waddle_keyboard_inhibit_event_t stale = make_event(WaddleKeyboardInhibitEventFocusEnter, 8, 0);
    assert(waddle_keyboard_inhibit_apply(&state, &stale, &result) == WaddleKeyboardInhibitInvalidIdentity);

    waddle_keyboard_inhibit_event_t destroy = make_event(WaddleKeyboardInhibitEventWindowDestroy, 8, 1);
    assert(waddle_keyboard_inhibit_apply(&state, &destroy, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 1 && result.actions[0].kind == WaddleKeyboardInhibitActionDestroy);
    assert(waddle_keyboard_inhibit_apply(&state, &destroy, &result) == WaddleKeyboardInhibitOk);
    assert(result.count == 0);
    for (size_t index = 0; index < WaddleKeyboardInhibitMaxActions; index++) {
        if (index >= result.count) assert(result.actions[index].kind == 0);
    }
}

static void test_table_capacity(void) {
    waddle_keyboard_inhibit_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_inhibit_result_t result;
    for (uint64_t window = 1; window <= WaddleKeyboardInhibitMaxTrackedWindows; window++) {
        waddle_keyboard_inhibit_event_t surface = make_surface(window, 1, 1, 0);
        assert(waddle_keyboard_inhibit_apply(&state, &surface, &result) == WaddleKeyboardInhibitOk);
    }
    waddle_keyboard_inhibit_event_t overflow = make_surface(99, 1, 1, 0);
    assert(waddle_keyboard_inhibit_apply(&state, &overflow, &result) == WaddleKeyboardInhibitWindowTableFull);
}

int main(void) {
    test_null_and_errors();
    test_default_policy_and_flags();
    test_session();
    test_table_capacity();
    return 0;
}
