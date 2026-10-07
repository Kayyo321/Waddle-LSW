/** @file win32_present.c @brief Native presentation pixels and GDI owner regression. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "waddle/venus_win32_present.h"
#include <assert.h>
#include <stdio.h>
/** @brief Native window regression; owns/destroys its HWND and every borrowed DC.
 * @return Zero after exact pixels, invalid-input checks and repeated GDI ownership.
 * @note Single UI thread; synchronous calls, no persistent dynamic allocation.
 */
int main(void) {
    uint32_t width = 7, height = 9;
    assert(!venus_win32_present_extent(0, &width, &height));
    assert(!width && !height);
    assert(!venus_win32_present_extent(0, &width, NULL));
    HWND window = CreateWindowExA(0, "STATIC", "Waddle presentation fixture",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  20, 20, 128, 128, NULL, NULL, GetModuleHandleA(NULL), NULL);
    assert(window);
    assert(UpdateWindow(window));
    assert(venus_win32_present_extent((uintptr_t)window, &width, &height));
    const uint32_t Pixels[4] = {0x00123456, 0x00123456, 0x00123456, 0x00123456};
    assert(!venus_win32_present_pixels((uintptr_t)window, 0, 2, Pixels, sizeof Pixels, 0));
    assert(!venus_win32_present_pixels((uintptr_t)window, 2, 2, Pixels, 15, 0));
    assert(!venus_win32_present_pixels((uintptr_t)window, 2, 2, Pixels, sizeof Pixels, 7));
    DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (unsigned cycle = 0; cycle < 128; cycle++) {
        assert(venus_win32_present_pixels((uintptr_t)window, 2, 2, Pixels, sizeof Pixels,
                                          cycle & 1 ? 2 : 0));
        HDC dc = GetDC(window);
        assert(dc);
        assert(GetPixel(dc, 1, 1) == RGB(0x12, 0x34, 0x56));
        assert(ReleaseDC(window, dc));
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi);
        assert(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == user);
    }
    assert(DestroyWindow(window));
    assert(!venus_win32_present_extent((uintptr_t)window, &width, &height));
    puts("Win32 presentation:128 exact BGRA pixels/FIFO/IMMEDIATE cycles; GDI/USER owners stable");
    return 0;
}
