/** @file win32_present_portable.c @brief Execute production native sink with deterministic APIs. */
#include "waddle/venus_win32_present.h"
#include <windows.h>
#include <dwmapi.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
/** @brief Single-thread test controls and exact native resource ledger. */
typedef struct fixture_t {
    RECT bounds;
    unsigned rect_calls, rect_fail_at, dc_calls, release_calls, draw_calls, flush_calls;
    int live, dc_live, dc_fail, release_fail, draw_fail, flush_fail;
    int resize_width, resize_height, lose_on_dc;
    int drawn_width, drawn_height;
} fixture_t;
static fixture_t fixture;
static const uint32_t Pixels[4] = {0x123456, 0x123456, 0x123456, 0x123456};

static void reset(void) {
    assert(!fixture.dc_live);
    memset(&fixture, 0, sizeof fixture);
    fixture.bounds = (RECT){0, 0, 2, 2};
    fixture.live = 1;
}
int IsWindow(HWND window) { return (uintptr_t)window == 1 && fixture.live; }
int GetClientRect(HWND window, RECT *bounds) {
    assert((uintptr_t)window == 1 && bounds);
    fixture.rect_calls++;
    if (fixture.rect_fail_at == fixture.rect_calls)
        return 0;
    *bounds = fixture.bounds;
    return 1;
}
HDC GetDC(HWND window) {
    assert((uintptr_t)window == 1 && !fixture.dc_live);
    fixture.dc_calls++;
    if (fixture.dc_fail)
        return NULL;
    fixture.dc_live = 1;
    if (fixture.resize_width)
        fixture.bounds.right++;
    if (fixture.resize_height)
        fixture.bounds.bottom++;
    if (fixture.lose_on_dc)
        fixture.live = 0;
    return (HDC)(uintptr_t)2;
}
int ReleaseDC(HWND window, HDC dc) {
    assert((uintptr_t)window == 1 && (uintptr_t)dc == 2 && fixture.dc_live);
    fixture.release_calls++;
    fixture.dc_live = 0;
    return !fixture.release_fail;
}
int StretchDIBits(HDC dc, int x, int y, int width, int height, int source_x, int source_y,
                 int source_width, int source_height, const void *pixels,
                 const BITMAPINFO *information, unsigned usage, unsigned operation) {
    assert((uintptr_t)dc == 2 && fixture.dc_live);
    assert(!x && !y && !source_x && !source_y && source_width == 2 && source_height == 2);
    assert(pixels == Pixels && information->bmiHeader.biSize == sizeof(BITMAPINFOHEADER));
    assert(information->bmiHeader.biWidth == 2 && information->bmiHeader.biHeight == -2);
    assert(information->bmiHeader.biPlanes == 1 && information->bmiHeader.biBitCount == 32);
    assert(information->bmiHeader.biCompression == BI_RGB && usage == DIB_RGB_COLORS);
    assert(operation == SRCCOPY);
    fixture.draw_calls++;
    fixture.drawn_width = width;
    fixture.drawn_height = height;
    return fixture.draw_fail ? 0 : source_height;
}
int32_t DwmFlush(void) {
    assert(!fixture.dc_live);
    fixture.flush_calls++;
    return fixture.flush_fail ? -1 : 0;
}
static int exact(unsigned mode) {
    return venus_win32_present_pixels_exact(1, 2, 2, Pixels, sizeof Pixels, mode);
}
static void test_extent(void) {
    reset();
    uint32_t width = 9, height = 9;
    assert(!venus_win32_present_extent(1, NULL, &height));
    assert(!venus_win32_present_extent(1, &width, NULL));
    assert(!venus_win32_present_extent(1, &width, &width));
    assert(!venus_win32_present_extent(0, &width, &height) && !width && !height);
    assert(!venus_win32_present_extent(2, &width, &height));
    fixture.rect_fail_at = 1;
    assert(!venus_win32_present_extent(1, &width, &height));
    reset(); fixture.bounds.right = -1;
    assert(!venus_win32_present_extent(1, &width, &height));
    reset(); fixture.bounds.bottom = -1;
    assert(!venus_win32_present_extent(1, &width, &height));
    reset(); fixture.bounds.right = 0;
    assert(venus_win32_present_extent(1, &width, &height) && !width && height == 2);
    reset(); fixture.bounds.bottom = 0;
    assert(venus_win32_present_extent(1, &width, &height) && width == 2 && !height);
    reset(); fixture.bounds.left = INT32_MIN; fixture.bounds.right = INT32_MAX;
    assert(venus_win32_present_extent(1, &width, &height) && width == UINT32_MAX);
}
static void test_parameters(void) {
    reset();
    assert(!exact(7));
    assert(!venus_win32_present_pixels_exact(1, 2, 2, NULL, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels_exact(1, 0, 2, Pixels, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels_exact(1, 2, 0, Pixels, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels_exact(1, 16385, 2, Pixels, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels_exact(1, 2, 16385, Pixels, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels_exact(1, 2, 2, Pixels, 15, 0));
    assert(!fixture.dc_calls);
}
static void test_native_failures(void) {
    reset(); fixture.live = 0; assert(!exact(0));
    reset(); fixture.bounds.left = INT32_MIN; fixture.bounds.right = INT32_MAX;
    assert(!exact(0));
    reset(); fixture.bounds.top = INT32_MIN; fixture.bounds.bottom = INT32_MAX;
    assert(!exact(0));
    reset(); fixture.bounds.right = 3; assert(exact(0) == -1 && !fixture.dc_calls);
    reset(); fixture.bounds.bottom = 3; assert(exact(0) == -1 && !fixture.dc_calls);
    reset(); fixture.bounds.right = 0; assert(exact(0) == -1 && !fixture.dc_calls);
    reset(); fixture.bounds.right = 0;
    assert(!venus_win32_present_pixels(1, 2, 2, Pixels, sizeof Pixels, 0));
    reset(); fixture.bounds.bottom = 0;
    assert(!venus_win32_present_pixels(1, 2, 2, Pixels, sizeof Pixels, 0));
    reset(); fixture.dc_fail = 1; assert(!exact(0) && !fixture.release_calls);
    reset(); fixture.resize_width = 1;
    assert(exact(0) == -1 && fixture.release_calls == 1 && !fixture.draw_calls);
    reset(); fixture.resize_height = 1;
    assert(exact(0) == -1 && fixture.release_calls == 1 && !fixture.draw_calls);
    reset(); fixture.lose_on_dc = 1;
    assert(!exact(0) && fixture.release_calls == 1 && !fixture.draw_calls);
    reset(); fixture.rect_fail_at = 2;
    assert(!exact(0) && fixture.release_calls == 1 && !fixture.draw_calls);
    reset(); fixture.resize_width = 1; fixture.release_fail = 1;
    assert(!exact(0) && fixture.release_calls == 1 && !fixture.draw_calls);
    reset(); fixture.draw_fail = 1;
    assert(!exact(0) && fixture.release_calls == 1 && !fixture.flush_calls);
    reset(); fixture.release_fail = 1;
    assert(!exact(0) && fixture.release_calls == 1 && !fixture.flush_calls);
    reset(); fixture.flush_fail = 1;
    assert(!exact(2) && fixture.release_calls == 1 && fixture.flush_calls == 1);
}
/** @brief Run native API-double regression; no heap, every acquired DC receives one release attempt.
 * @return Zero after bounded success/error/resize coverage. Single test thread.
 */
int main(void) {
    test_extent();
    test_parameters();
    test_native_failures();
    for (unsigned cycle = 0; cycle < 128; cycle++) {
        reset();
        unsigned mode = cycle & 1 ? 2 : 0;
        assert(exact(mode) == 1);
        assert(fixture.rect_calls == 2 && fixture.release_calls == 1 && !fixture.dc_live);
        assert(fixture.drawn_width == 2 && fixture.drawn_height == 2);
        assert(fixture.flush_calls == (mode == 2));
    }
    reset(); fixture.bounds.right = 4; fixture.bounds.bottom = 6;
    assert(venus_win32_present_pixels(1, 2, 2, Pixels, sizeof Pixels, 0));
    assert(fixture.drawn_width == 4 && fixture.drawn_height == 6 && fixture.release_calls == 1);
    puts("Win32 API doubles: exact-size, minimized, resize/loss during DC acquisition; balanced acquire/release attempts");
    return 0;
}

#else
/** @brief Verify unsupported native calls fail without touching borrowed input.
 * @return Zero after all public calls reject a non-Windows target. Single thread.
 */
int main(void) {
    uint32_t width = 7, height = 9, pixel = 0;
    assert(!venus_win32_present_extent(1, &width, &height) && !width && !height);
    assert(!venus_win32_present_pixels(1, 1, 1, &pixel, sizeof pixel, 0));
    assert(!venus_win32_present_pixels_exact(1, 1, 1, &pixel, sizeof pixel, 0));
    puts("Win32 sink: unsupported platform fails without native side effects");
    return 0;
}
#endif
