/** @file keyboard_repeat_abi.c @brief Independent C consumer for the host key-repeat C ABI. */
#include "keyboard_repeat_abi.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(waddle_keyboard_repeat_t) == WaddleKeyboardRepeatStorageBytes, "storage size");
_Static_assert(_Alignof(waddle_keyboard_repeat_t) == 8, "storage alignment");

static void test_null_and_flags(void) {
    waddle_keyboard_repeat_t storage;
    memset(&storage, 0, sizeof(storage));
    assert(waddle_keyboard_repeat_init(NULL) == WaddleKeyboardRepeatNullArgument);
    assert(waddle_keyboard_repeat_configure(&storage, 2, 30, 100) == WaddleKeyboardRepeatInvalidFlag);
    assert(waddle_keyboard_repeat_configure(&storage, 1, 1001, 100) == WaddleKeyboardRepeatInvalidTiming);
    assert(waddle_keyboard_repeat_configure(&storage, 1, 30, -1) == WaddleKeyboardRepeatInvalidTiming);
    assert(waddle_keyboard_repeat_configure(&storage, 1, 30, 100) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_focus(&storage, 0, 1) == WaddleKeyboardRepeatInvalidFocus);
    assert(waddle_keyboard_repeat_focus(&storage, 1, 0) == WaddleKeyboardRepeatInvalidFocus);
    assert(waddle_keyboard_repeat_focus(&storage, 1, 1) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_key(&storage, 0, 1, 1, 0) == WaddleKeyboardRepeatInvalidKey);
    assert(waddle_keyboard_repeat_key(&storage, 768, 1, 1, 0) == WaddleKeyboardRepeatInvalidKey);
    assert(waddle_keyboard_repeat_key(&storage, 30, 3, 1, 0) == WaddleKeyboardRepeatInvalidFlag);
    assert(waddle_keyboard_repeat_key(&storage, 30, 1, 5, 0) == WaddleKeyboardRepeatInvalidFlag);
    assert(waddle_keyboard_repeat_key(NULL, 30, 1, 1, 0) == WaddleKeyboardRepeatNullArgument);
    uint8_t present = 9;
    uint64_t deadline = 9;
    assert(waddle_keyboard_repeat_next_deadline(&storage, &present, &deadline) == WaddleKeyboardRepeatOk);
    assert(present == 0 && deadline == 0);
    assert(waddle_keyboard_repeat_next_deadline(&storage, NULL, &deadline) == WaddleKeyboardRepeatNullArgument);
    assert(waddle_keyboard_repeat_next_deadline(&storage, &present, NULL) == WaddleKeyboardRepeatNullArgument);
}

static void test_exact_repeat_and_cancel(void) {
    waddle_keyboard_repeat_t storage;
    assert(waddle_keyboard_repeat_init(&storage) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_configure(&storage, 1, 4, 20) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_focus(&storage, 9, 3) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_key(&storage, 30, 1, 1, 1000000000ull) == WaddleKeyboardRepeatOk);

    uint8_t present = 0;
    uint64_t deadline = 0;
    assert(waddle_keyboard_repeat_next_deadline(&storage, &present, &deadline) == WaddleKeyboardRepeatOk);
    assert(present == 1 && deadline == 1020000000ull);

    uint16_t code = 77;
    assert(waddle_keyboard_repeat_take_due(&storage, 1019999999ull, &code) == WaddleKeyboardRepeatOk);
    assert(code == 0);
    assert(waddle_keyboard_repeat_take_due(&storage, 1020000000ull, &code) == WaddleKeyboardRepeatOk);
    assert(code == 30);
    assert(waddle_keyboard_repeat_next_deadline(&storage, &present, &deadline) == WaddleKeyboardRepeatOk);
    assert(present == 1 && deadline == 1270000000ull);

    assert(waddle_keyboard_repeat_take_due(&storage, 1270000000ull, &code) == WaddleKeyboardRepeatOk);
    assert(code == 30);
    assert(waddle_keyboard_repeat_cancel(&storage) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_cancel(&storage) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_take_due(&storage, 2000000000ull, &code) == WaddleKeyboardRepeatOk);
    assert(code == 0);
    assert(waddle_keyboard_repeat_take_due(&storage, 2000000000ull, NULL) == WaddleKeyboardRepeatNullArgument);
}

static void test_clock_rollback_disarms(void) {
    waddle_keyboard_repeat_t storage;
    assert(waddle_keyboard_repeat_init(&storage) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_configure(&storage, 1, 30, 100) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_focus(&storage, 1, 1) == WaddleKeyboardRepeatOk);
    assert(waddle_keyboard_repeat_key(&storage, 31, 1, 1, 500ull) == WaddleKeyboardRepeatOk);
    uint16_t code = 5;
    assert(waddle_keyboard_repeat_take_due(&storage, 499ull, &code) == WaddleKeyboardRepeatClockDiscontinuity);
    assert(code == 5);
    uint8_t present = 1;
    uint64_t deadline = 1;
    assert(waddle_keyboard_repeat_next_deadline(&storage, &present, &deadline) == WaddleKeyboardRepeatOk);
    assert(present == 0);
}

int main(void) {
    test_null_and_flags();
    test_exact_repeat_and_cancel();
    test_clock_rollback_disarms();
    return 0;
}
