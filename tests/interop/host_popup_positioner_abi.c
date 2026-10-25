/**
 * @file host_popup_positioner_abi.c
 * @brief C consumer checks for the popup positioner C ABI (`src/interop/host_popup_positioner.h`).
 *
 * Built with -fsanitize=address,leak,undefined and linked against the Zig object. The positioner
 * allocates nothing, so every object here is caller-owned stack storage.
 */
#include "host_popup_positioner.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "host_popup_positioner_abi.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static host_popup_rect_t rect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    host_popup_rect_t value;
    value.x = x;
    value.y = y;
    value.width = width;
    value.height = height;
    return value;
}

static int same(host_popup_rect_t left, host_popup_rect_t right) {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

static int check_constants(void) {
    Check(HostPopupMaxScale == 8);
    Check(HostPopupMaxLogicalExtent == 32768);
    Check(HostPopupAnchorTopLeft == 5);
    Check(HostPopupGravityBottomRight == 8);
    Check(HostPopupConstraintSlideFlip == 15);
    Check(sizeof(host_popup_rect_t) == 16);
    return 0;
}

static int check_integer_scale(void) {
    uint32_t scale = 99;
    Check(host_popup_positioner_integer_scale(1, 1, NULL) == HostPopupInvalidArgument);
    Check(host_popup_positioner_integer_scale(1, 0, &scale) == HostPopupInvalidScale);
    Check(scale == 99);
    Check(host_popup_positioner_integer_scale(961, 120, &scale) == HostPopupInvalidScale);
    Check(scale == 99);
    Check(host_popup_positioner_integer_scale(150, 120, &scale) == HostPopupOk);
    Check(scale == 2);
    Check(host_popup_positioner_integer_scale(120, 120, &scale) == HostPopupOk);
    Check(scale == 1);
    return 0;
}

static int check_request(void) {
    host_popup_rect_t guest;
    host_popup_request_t out;
    memset(&out, 0x5A, sizeof out);
    out.width = 7;

    Check(host_popup_positioner_request(NULL, 1, 10, 10, &out) == HostPopupInvalidArgument);
    guest = rect(0, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, 10, 10, NULL) == HostPopupInvalidArgument);
    Check(host_popup_positioner_request(&guest, 9, 10, 10, &out) == HostPopupInvalidScale);
    Check(host_popup_positioner_request(&guest, 1, 0, 10, &out) == HostPopupInvalidParent);
    Check(host_popup_positioner_request(&guest, 1, 10, 32769, &out) == HostPopupOutOfRange);
    Check(out.width == 7);

    /* Unmoved: the origin lies inside the parent, so the offset is zero. */
    guest = rect(30, 12, 40, 20);
    Check(host_popup_positioner_request(&guest, 2, 1920, 1080, &out) == HostPopupOk);
    Check(out.anchor_x == 15 && out.anchor_y == 6);
    Check(out.offset_x == 0 && out.offset_y == 0);
    Check(out.width == 20 && out.height == 10);
    Check(out.anchor_width == 1 && out.anchor_height == 1);
    Check(out.anchor == HostPopupAnchorTopLeft);
    Check(out.gravity == HostPopupGravityBottomRight);
    Check(out.constraint_adjustment == HostPopupConstraintSlideFlip);

    /* Negative origin: clamped to the parent edge, the remainder is the offset. */
    guest = rect(-7, -3, 4, 4);
    Check(host_popup_positioner_request(&guest, 2, 100, 100, &out) == HostPopupOk);
    Check(out.anchor_x == 0 && out.anchor_y == 0);
    Check(out.offset_x == -4 && out.offset_y == -2);

    /* Past the far edge: clamped to parent size minus one. */
    guest = rect(1000, 500, 4, 4);
    Check(host_popup_positioner_request(&guest, 1, 100, 60, &out) == HostPopupOk);
    Check(out.anchor_x == 99 && out.anchor_y == 59);
    Check(out.offset_x == 901 && out.offset_y == 441);

    /* Nested: a popup inside a 300 by 200 menu is clamped against the menu, not the toplevel. */
    guest = rect(40, 450, 40, 40);
    Check(host_popup_positioner_request(&guest, 2, 300, 200, &out) == HostPopupOk);
    Check(out.anchor_y == 199 && out.offset_y == 26);
    Check(host_popup_positioner_request(&guest, 2, 1920, 1080, &out) == HostPopupOk);
    Check(out.anchor_y == 225 && out.offset_y == 0);

    /* Origin exactly at 32768 is accepted; 32769 is refused. */
    guest = rect(32768, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, 1, 1, &out) == HostPopupOk);
    Check(out.offset_x == 32768);
    guest = rect(32769, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, 1, 1, &out) == HostPopupOutOfRange);
    Check(out.offset_x == 32768);
    return 0;
}

static int check_restore(void) {
    host_popup_rect_t logical;
    host_popup_rect_t out = rect(7, 7, 7, 7);

    Check(host_popup_positioner_restore(NULL, 1, &out) == HostPopupInvalidArgument);
    logical = rect(0, 0, 1, 1);
    Check(host_popup_positioner_restore(&logical, 1, NULL) == HostPopupInvalidArgument);
    Check(host_popup_positioner_restore(&logical, 0, &out) == HostPopupInvalidScale);

    logical = rect(268435455, 0, 0, 0);
    Check(host_popup_positioner_restore(&logical, 8, &out) == HostPopupOk);
    Check(out.x == 2147483640);

    logical = rect(268435456, 0, 0, 0);
    Check(host_popup_positioner_restore(&logical, 8, &out) == HostPopupOverflow);
    Check(out.x == 2147483640);

    logical = rect(0, 0, 2147483648u, 0);
    Check(host_popup_positioner_restore(&logical, 2, &out) == HostPopupOverflow);
    logical = rect(0, 0, 2147483647u, 0);
    Check(host_popup_positioner_restore(&logical, 2, &out) == HostPopupOk);
    Check(out.width == 4294967294u);

    logical = rect(-3, 4, 5, 6);
    Check(host_popup_positioner_restore(&logical, 2, &out) == HostPopupOk);
    Check(same(out, rect(-6, 8, 10, 12)));
    return 0;
}

int main(void) {
    if (check_constants() != 0) return 1;
    if (check_integer_scale() != 0) return 1;
    if (check_request() != 0) return 1;
    if (check_restore() != 0) return 1;
    puts("host_popup_positioner_abi: all checks passed");
    return 0;
}
