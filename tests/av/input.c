/** @file input.c @brief Portable input ownership and failure fixtures. */
#include "av_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture_t {
    uint64_t allowed_id, incarnation;
    unsigned emitted, activated, releases;
    int fail_target, fail_emit, stale_target, fail_activate;
    uint32_t fail_code;
    av_message_t last;
} fixture_t;
static int target(void *context, uint64_t id, uint64_t incarnation, int activate) {
    fixture_t *fixture = context;
    if (activate < 0 && fixture->stale_target) return 1;
    if ((activate > 0 && fixture->fail_activate) || fixture->fail_target || id != fixture->allowed_id || incarnation != fixture->incarnation)
        return -1;
    if (activate > 0) ++fixture->activated;
    return 0;
}
static int emit(void *context, const av_message_t *event) {
    fixture_t *fixture = context;
    ++fixture->emitted;
    fixture->last = *event;
    if (fixture->fail_emit || (fixture->fail_code && event->width == fixture->fail_code)) return -1;
    if (!event->flags && (event->type == MsgInputKey || event->type == MsgInputButton))
        ++fixture->releases;
    return 0;
}
static const av_input_ops_t Ops = {.target = target, .emit = emit};
static av_message_t event(uint32_t type, uint32_t serial, uint32_t code, uint32_t down) {
    av_message_t result = {.type = type, .window_id = 7, .sequence = 99,
        .buffer_index = serial, .width = code, .flags = down};
    return result;
}
static void invalid_arguments(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    av_input_ops_t incomplete = {0};
    assert(av_input_apply(NULL, &message, &Ops, &fixture) == -1);
    assert(av_input_apply(&state, NULL, &Ops, &fixture) == -1);
    assert(av_input_apply(&state, &message, NULL, &fixture) == -1);
    assert(av_input_apply(&state, &message, &incomplete, &fixture) == -1);
    incomplete.target = target;
    assert(av_input_apply(&state, &message, &incomplete, &fixture) == -1);
    assert(av_input_reset(NULL, &Ops, &fixture) == -1);
    assert(av_input_reset(&state, NULL, &fixture) == -1);
    assert(av_input_reset(&state, &incomplete, &fixture) == -1);
    message.type = MsgWindowClose;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    message.type = MsgInputRelease + 1;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    message.type = MsgInputFocus;
    message.sequence = 0;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    assert(state.serial == 0 && fixture.emitted == 0);
}
static void ownership(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputKey, 1, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0); /* no focus: stale input cannot inject */
    message = event(MsgInputFocus, 2, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(state.window_id == 7 && state.incarnation == 99 && fixture.activated == 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1); /* replay */
    message = event(MsgInputKey, 3, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.keys[30]);
    ++message.buffer_index;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 1);
    message = event(MsgInputButton, 5, 1, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.buttons[1]);
    message = event(MsgInputRelease, 6, 0, AvInputReleaseButtons);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(state.keys[30] && !state.buttons[1] && state.window_id == 7);
    message = event(MsgInputRelease, 7, 0, AvInputReleaseKeys);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && !state.keys[30]);
    message = event(MsgInputKey, 8, 30, 0);
    unsigned count = fixture.emitted;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == count);
    message = event(MsgInputPointer, 9, 0, 0);
    message.x = 100; message.y = 200;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputWheel, 10, 0, 0); message.y = -120;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputFocus, 11, 0, 0);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && !state.window_id);
    assert(fixture.releases == 2);
}
static void failures(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    fixture.fail_target = 1;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && !state.window_id);
    fixture.fail_target = 0; ++message.buffer_index;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputKey, 3, 30, 1); message.sequence = 98;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && !state.keys[30]);
    message.sequence = 99; ++message.buffer_index; fixture.fail_target = 1;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && !state.keys[30]);
    fixture.fail_target = 0; fixture.fail_emit = 1; ++message.buffer_index;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && !state.keys[30]);
    fixture.fail_emit = 0; ++message.buffer_index;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.keys[30]);
    message = event(MsgInputButton, 7, 3, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.buttons[3]);
    fixture.fail_emit = 1;
    assert(av_input_reset(&state, &Ops, &fixture) == -1);
    assert(state.keys[30] && state.buttons[3] && state.window_id == 7);
    message = event(MsgInputFocus, 8, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    fixture.fail_emit = 0;
    assert(av_input_reset(&state, &Ops, &fixture) == 0);
    assert(!state.keys[30] && !state.buttons[3] && !state.window_id);
    assert(av_input_reset(&state, &Ops, &fixture) == 0);
    message = event(MsgInputFocus, 9, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputKey, 10, 84, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    message = event(MsgInputFocus, 11, 0, 1);
    message.window_id = 8; message.sequence = 100;
    fixture.allowed_id = 8; fixture.incarnation = 100;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.window_id == 8);
    message = event(MsgInputKey, 12, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(av_input_reset(&state, &Ops, &fixture) == 0);
}
static void all_keys_and_stress(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    uint32_t serial = 0;
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        av_message_t message = event(MsgInputFocus, ++serial, 0, 1);
        assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
        for (unsigned key = 1; key < AvInputKeys; ++key) {
            message = event(MsgInputKey, ++serial, key, 1);
            assert(av_input_apply(&state, &message, &Ops, &fixture) == (av_input_scan_code(key) ? 0 : -1));
        }
        for (unsigned button = 1; button <= 3; ++button) {
            message = event(MsgInputButton, ++serial, button, 1);
            assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
        }
        assert(av_input_reset(&state, &Ops, &fixture) == 0);
        for (unsigned key = 0; key < AvInputKeys; ++key) assert(!state.keys[key]);
        for (unsigned button = 0; button < 4; ++button) assert(!state.buttons[button]);
    }
    assert(!av_input_scan_code(0) && !av_input_scan_code(128) && !av_input_scan_code(UINT32_MAX));
    assert(av_input_scan_code(30) == 0x1e && av_input_scan_code(97) == 0x11d);
}
static void transfer_and_partial_release(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputKey, 2, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputKey, 3, 31, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputButton, 4, 1, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    fixture.fail_code = 30;
    assert(av_input_reset(&state, &Ops, &fixture) == -1);
    assert(state.keys[30] && !state.keys[31] && !state.buttons[1]);
    fixture.fail_code = 0; fixture.fail_activate = 1;
    message = event(MsgInputFocus, 5, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    assert(!state.window_id && !state.keys[30]);
    fixture.fail_activate = 0;
    message = event(MsgInputFocus, 6, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    fixture.stale_target = 1;
    message = event(MsgInputFocus, 7, 0, 1); message.sequence = 98;
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(state.incarnation == 99); /* stale activation cannot release current focus */
    fixture.stale_target = 0;
    message = event(MsgInputFocus, UINT32_MAX, 0, 0);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(av_input_reset(&state, &Ops, &fixture) == 0 && state.serial == UINT32_MAX);
    message = event(MsgInputFocus, 1, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
}
int main(void) {
    invalid_arguments(); ownership(); failures(); all_keys_and_stress(); transfer_and_partial_release();
    puts("AV input ownership, replay, failure and release fixtures passed");
    return 0;
}
