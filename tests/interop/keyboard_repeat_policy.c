/** @file keyboard_repeat_policy.c @brief Parity of the host (Zig) and guest (C) repeat policy over every evdev code. */
#include "keyboard_repeat_abi.h"
#include "av_input.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* The golden exclusion list is pinned here as well. Editing either implementation without
 * this list (and the other implementation) fails the parity or golden assertion below. */
static const uint16_t NotRepeatable[] = {29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 122, 123, 125, 126, 142};

int main(void) {
    unsigned repeatable = 0;
    for (uint32_t key = 0; key <= 767; ++key) {
        int host = waddle_keyboard_repeat_key_repeatable((uint16_t)key);
        int guest = av_input_key_repeatable(key);
        assert(host == guest);
        if (host) ++repeatable;
    }
    /* Codes 1..767 are 767 keys; 16 are excluded. Code 0 is not a key. */
    assert(repeatable == 751);
    for (size_t index = 0; index < sizeof(NotRepeatable) / sizeof(NotRepeatable[0]); ++index) {
        assert(!waddle_keyboard_repeat_key_repeatable(NotRepeatable[index]));
        assert(!av_input_key_repeatable(NotRepeatable[index]));
    }
    assert(waddle_keyboard_repeat_key_repeatable(30) && av_input_key_repeatable(30));
    puts("keyboard repeat policy: host and guest agree for every evdev code 0..767; golden exclusions pinned");
    return 0;
}
