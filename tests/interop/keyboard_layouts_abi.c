/** @file keyboard_layouts_abi.c @brief Independent C consumer for the keyboard layout C ABI. */
#include "keyboard_layouts.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(waddle_keyboard_layouts_klid_t) == 8, "klid layout");
_Static_assert(sizeof(waddle_keyboard_layouts_state_t) == 9, "state layout");
_Static_assert(sizeof(waddle_keyboard_layouts_outcome_t) == 9, "outcome layout");

static int klid_is(const waddle_keyboard_layouts_klid_t *klid, const char *text) {
    return memcmp(klid->bytes, text, WaddleKeyboardLayoutsKlidBytes) == 0;
}

static void test_klid_text(void) {
    waddle_keyboard_layouts_klid_t klid;
    memset(&klid, 0, sizeof(klid));
    const uint8_t mixed[] = "0000abcd";
    assert(waddle_keyboard_layouts_validate_klid(NULL, 8, &klid) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_validate_klid(mixed, 8, NULL) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_validate_klid(mixed, 7, &klid) == WaddleKeyboardLayoutsInvalidKlid);
    assert(waddle_keyboard_layouts_validate_klid(mixed, 9, &klid) == WaddleKeyboardLayoutsInvalidKlid);
    assert(waddle_keyboard_layouts_validate_klid((const uint8_t *)"00000000", 8, &klid) == WaddleKeyboardLayoutsInvalidKlid);
    assert(waddle_keyboard_layouts_validate_klid((const uint8_t *)"0000040g", 8, &klid) == WaddleKeyboardLayoutsInvalidKlid);
    assert(klid_is(&klid, "\0\0\0\0\0\0\0\0"));
    assert(waddle_keyboard_layouts_validate_klid(mixed, 8, &klid) == WaddleKeyboardLayoutsOk);
    assert(klid_is(&klid, "0000ABCD"));
}

static void test_observe_sequence(void) {
    waddle_keyboard_layouts_state_t state;
    memset(&state, 0, sizeof(state));
    waddle_keyboard_layouts_outcome_t outcome;
    memset(&outcome, 0x7e, sizeof(outcome));

    const uint8_t us[] = "us";
    const uint8_t intl[] = "intl";
    assert(waddle_keyboard_layouts_observe(NULL, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_observe(&state, us, 2, NULL, 0, NULL) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_observe(&state, NULL, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_observe(&state, us, 2, NULL, 4, &outcome) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_observe(&state, us, 0, NULL, 0, &outcome) == WaddleKeyboardLayoutsInvalidName);
    assert(waddle_keyboard_layouts_observe(&state, (const uint8_t *)"US", 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsInvalidName);
    assert(!state.has_applied);
    assert(outcome.kind == 0x7e);

    assert(waddle_keyboard_layouts_observe(&state, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsSend && klid_is(&outcome.klid, "00000409"));
    assert(waddle_keyboard_layouts_observe(&state, us, 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsUnchanged && klid_is(&outcome.klid, "00000409"));
    assert(waddle_keyboard_layouts_observe(&state, us, 2, intl, 4, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsSend && klid_is(&outcome.klid, "00020409"));

    assert(waddle_keyboard_layouts_observe(&state, (const uint8_t *)"zz", 2, NULL, 0, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsNoMapping);
    for (size_t index = 0; index < sizeof(outcome.klid.bytes); index++) assert(outcome.klid.bytes[index] == 0);
    assert(state.has_applied && memcmp(state.applied.bytes, "00020409", WaddleKeyboardLayoutsKlidBytes) == 0);

    assert(waddle_keyboard_layouts_observe(&state, us, 2, intl, 4, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsUnchanged);

    assert(waddle_keyboard_layouts_reset(NULL) == WaddleKeyboardLayoutsNullArgument);
    assert(waddle_keyboard_layouts_reset(&state) == WaddleKeyboardLayoutsOk);
    assert(!state.has_applied);
    assert(waddle_keyboard_layouts_observe(&state, intl, 4, NULL, 0, &outcome) == WaddleKeyboardLayoutsOk);
    assert(outcome.kind == WaddleKeyboardLayoutsNoMapping);
}

int main(void) {
    test_klid_text();
    test_observe_sequence();
    return 0;
}
