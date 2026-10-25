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
    host_popup_rect_t out = rect(7, 7, 7, 7);

    Check(host_popup_positioner_request(NULL, 1, &out) == HostPopupInvalidArgument);
    guest = rect(0, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, NULL) == HostPopupInvalidArgument);
    Check(host_popup_positioner_request(&guest, 9, &out) == HostPopupInvalidScale);
    Check(same(out, rect(7, 7, 7, 7)));

    guest = rect(-8, 16, 9, 17);
    Check(host_popup_positioner_request(&guest, 8, &out) == HostPopupOk);
    Check(same(out, rect(-1, 2, 2, 3)));

    guest = rect(-3, -3, 1, 1);
    Check(host_popup_positioner_request(&guest, 2, &out) == HostPopupOk);
    Check(same(out, rect(-2, -2, 1, 1)));

    guest = rect(32768, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, &out) == HostPopupOk);
    Check(out.x == 32768);
    guest = rect(32769, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 1, &out) == HostPopupOutOfRange);
    Check(out.x == 32768);
    guest = rect(-65537, 0, 1, 1);
    Check(host_popup_positioner_request(&guest, 2, &out) == HostPopupOutOfRange);
    guest = rect(0, 0, 4294967295u, 1);
    Check(host_popup_positioner_request(&guest, 8, &out) == HostPopupOutOfRange);
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
