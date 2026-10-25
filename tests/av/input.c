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
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 2); /* repeatable duplicate */
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
            assert(av_input_apply(&state, &message, &Ops, &fixture) == (av_input_key_injectable(key) ? 0 : -1));
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
static void repeat_admission(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputKey, 2, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 1 && state.keys[30] == 1);
    /* Host typematic repeats: each is a plain down while the key stays held. */
    for (uint32_t serial = 3; serial <= 6; ++serial) {
        message = event(MsgInputKey, serial, 30, 1);
        assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    }
    assert(fixture.emitted == 5 && state.keys[30] == 1 && fixture.last.flags == 1);
    /* Duplicate downs for modifier and lock keys stay suppressed. */
    message = event(MsgInputKey, 7, 42, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 6);
    message = event(MsgInputKey, 8, 42, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 6);
    message = event(MsgInputKey, 9, 58, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 7);
    message = event(MsgInputKey, 10, 58, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 7);
    /* A release clears the held bit, so the next down is a fresh press, not a repeat. */
    message = event(MsgInputKey, 11, 30, 0);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 8 && !state.keys[30]);
    message = event(MsgInputKey, 12, 30, 0);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 8);
    message = event(MsgInputKey, 13, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && fixture.emitted == 9 && state.keys[30] == 1);
    /* Repeat emit failure keeps the key held and reports failure. */
    fixture.fail_emit = 1;
    message = event(MsgInputKey, 14, 30, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && state.keys[30] == 1);
    fixture.fail_emit = 0;
    message = event(MsgInputRelease, 15, 0, AvInputReleaseKeys);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && !state.keys[30]);
}
static void repeat_policy(void) {
    assert(av_input_key_repeatable(30) && av_input_key_repeatable(1) && av_input_key_repeatable(767));
    assert(av_input_key_repeatable(0) == 0 && av_input_key_repeatable(768) == 0);
    static const uint32_t NotRepeatable[] = {29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 125, 126, 142};
    for (size_t index = 0; index < sizeof(NotRepeatable) / sizeof(NotRepeatable[0]); ++index)
        assert(av_input_key_repeatable(NotRepeatable[index]) == 0);
}
static void typed_admission(void) {
    assert(av_input_key_injectable(30) && av_input_key_injectable(96) && av_input_key_injectable(122));
    assert(av_input_key_injectable(123) && av_input_key_injectable(119));
    assert(!av_input_key_injectable(0) && !av_input_key_injectable(116) && !av_input_key_injectable(142));
    assert(!av_input_key_injectable(118) && !av_input_key_injectable(128));
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    /* Typed keys: Hangul, Hanja and Pause are admitted and held until released. */
    message = event(MsgInputKey, 2, 122, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.keys[122] == 1);
    message = event(MsgInputKey, 3, 123, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.keys[123] == 1);
    message = event(MsgInputKey, 4, 119, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.keys[119] == 1);
    /* Suppressed and unsupported keys are rejected and never held. */
    message = event(MsgInputKey, 5, 116, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && !state.keys[116]);
    message = event(MsgInputKey, 6, 118, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1 && !state.keys[118]);
    unsigned count = fixture.emitted;
    message = event(MsgInputRelease, 7, 0, AvInputReleaseKeys);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(!state.keys[122] && !state.keys[123] && !state.keys[119] && fixture.emitted == count + 3);
}
static void xbutton_admission(void) {
    fixture_t fixture = {.allowed_id = 7, .incarnation = 99};
    av_input_state_t state = {0};
    av_message_t message = event(MsgInputFocus, 1, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    message = event(MsgInputButton, 2, 4, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.buttons[4] == 1);
    message = event(MsgInputButton, 3, 5, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0 && state.buttons[5] == 1);
    message = event(MsgInputButton, 4, 6, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    unsigned held = 0;
    for (unsigned button = 0; button < sizeof(state.buttons); ++button) held += state.buttons[button];
    assert(held == 2); /* the rejected width 6 left nothing held */
    message = event(MsgInputButton, 5, 0, 1);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == -1);
    /* Releasing all buttons clears both XBUTTONs with their up events. */
    unsigned count = fixture.emitted;
    message = event(MsgInputRelease, 6, 0, AvInputReleaseButtons);
    assert(av_input_apply(&state, &message, &Ops, &fixture) == 0);
    assert(!state.buttons[4] && !state.buttons[5] && fixture.emitted == count + 2);
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
    invalid_arguments(); ownership(); repeat_admission(); repeat_policy(); typed_admission(); xbutton_admission(); failures(); all_keys_and_stress(); transfer_and_partial_release();
    puts("AV input ownership, replay, failure and release fixtures passed");
    return 0;
}
