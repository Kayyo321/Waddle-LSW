/** @file keyboard_layout_abi.c @brief Independent C consumer for the keyboard layout C ABI. */
#include "keyboard_layout.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(waddle_keyboard_layout_klid_t) == 8, "klid layout");
_Static_assert(sizeof(waddle_keyboard_layout_state_t) == 9, "state layout");
_Static_assert(sizeof(waddle_keyboard_layout_outcome_t) == 9, "outcome layout");

static int klid_is(const waddle_keyboard_layout_klid_t *klid, const char *text) {
    return memcmp(klid->bytes, text, WaddleKeyboardLayoutKlidBytes) == 0;
}

static void test_klid_text(void) {
    waddle_keyboard_layout_klid_t klid;
    memset(&klid, 0, sizeof(klid));
    const uint8_t mixed[] = "0000abcd";
    assert(waddle_keyboard_layout_validate_klid(NULL, 8, &klid) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_validate_klid(mixed, 8, NULL) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_validate_klid(mixed, 7, &klid) == WaddleKeyboardLayoutInvalidKlid);
    assert(waddle_keyboard_layout_validate_klid(mixed, 9, &klid) == WaddleKeyboardLayoutInvalidKlid);
    assert(waddle_keyboard_layout_validate_klid((const uint8_t *)"00000000", 8, &klid) == WaddleKeyboardLayoutInvalidKlid);
    assert(waddle_keyboard_layout_validate_klid((const uint8_t *)"0000040g", 8, &klid) == WaddleKeyboardLayoutInvalidKlid);
    assert(klid_is(&klid, "\0\0\0\0\0\0\0\0"));
    assert(waddle_keyboard_layout_validate_klid(mixed, 8, &klid) == WaddleKeyboardLayoutOk);
    assert(klid_is(&klid, "0000ABCD"));
}

static void test_observe_sequence(void) {
    waddle_keyboard_layout_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_layout_outcome_t outcome;
    memset(&outcome, 0x7e, sizeof(outcome));

    const uint8_t us[] = "us";
    const uint8_t intl[] = "intl";
    assert(waddle_keyboard_layout_observe(NULL, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_observe(&state, us, 2, NULL, 0, NULL) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_observe(&state, NULL, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_observe(&state, us, 2, NULL, 4, &outcome) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_observe(&state, us, 0, NULL, 0, &outcome) == WaddleKeyboardLayoutInvalidName);
    assert(waddle_keyboard_layout_observe(&state, (const uint8_t *)"US", 2, NULL, 0, &outcome) == WaddleKeyboardLayoutInvalidName);
    assert(!state.has_applied);
    assert(outcome.kind == 0x7e);

    assert(waddle_keyboard_layout_observe(&state, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutSend && klid_is(&outcome.klid, "00000409"));
    assert(waddle_keyboard_layout_observe(&state, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutUnchanged && klid_is(&outcome.klid, "00000409"));
    assert(waddle_keyboard_layout_observe(&state, us, 2, intl, 4, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutSend && klid_is(&outcome.klid, "00020409"));

    assert(waddle_keyboard_layout_observe(&state, (const uint8_t *)"zz", 2, NULL, 0, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutNoMapping);
    for (size_t index = 0; index < sizeof(outcome.klid.bytes); index++) assert(outcome.klid.bytes[index] == 0);
    assert(state.has_applied && memcmp(state.applied.bytes, "00020409", WaddleKeyboardLayoutKlidBytes) == 0);

    assert(waddle_keyboard_layout_observe(&state, us, 2, intl, 4, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutUnchanged);

    assert(waddle_keyboard_layout_reset(NULL) == WaddleKeyboardLayoutNullArgument);
    assert(waddle_keyboard_layout_reset(&state) == WaddleKeyboardLayoutOk);
    assert(!state.has_applied);
    assert(waddle_keyboard_layout_observe(&state, intl, 4, NULL, 0, &outcome) == WaddleKeyboardLayoutOk);
    assert(outcome.kind == WaddleKeyboardLayoutNoMapping);
}

int main(void) {
    test_klid_text();
    test_observe_sequence();
    return 0;
}
