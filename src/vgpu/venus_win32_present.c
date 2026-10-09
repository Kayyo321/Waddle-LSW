/** @file venus_win32_present.c @brief Bounded synchronous Win32 window pixel sink. */
#include "waddle/venus_win32_present.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#endif
int venus_win32_present_extent(uintptr_t hwnd, uint32_t *width, uint32_t *height) {
    if (!width || !height || width == height)
        return 0;
    *width = *height = 0;
#ifdef _WIN32
    HWND window = (HWND)hwnd;
    RECT bounds;
    if (!window || !IsWindow(window) || !GetClientRect(window, &bounds) ||
        bounds.right < bounds.left || bounds.bottom < bounds.top)
        return 0;
    *width = (uint32_t)((int64_t)bounds.right - bounds.left);
    *height = (uint32_t)((int64_t)bounds.bottom - bounds.top);
    return 1;
#else
    (void)hwnd;
    return 0;
#endif
}
static int present_pixels(uintptr_t hwnd, uint32_t width, uint32_t height,
                          const void *pixels, size_t bytes, uint32_t present_mode, int exact) {
    if ((present_mode != 0 && present_mode != 2) || !pixels || !width || !height || width > 16384 || height > 16384 ||
        (size_t)width > SIZE_MAX / 4 / height || bytes < (size_t)width * height * 4)
        return 0;
#ifdef _WIN32
    uint32_t client_width, client_height;
    if (!venus_win32_present_extent(hwnd, &client_width, &client_height) ||
        client_width > INT32_MAX || client_height > INT32_MAX)
        return 0;
    if (exact && (client_width != width || client_height != height))
        return -1;
    if (!client_width || !client_height)
        return 0;
    HWND window = (HWND)hwnd;
    HDC dc = GetDC(window);
    if (!dc)
        return 0;
    if (exact) {
        int status = venus_win32_present_extent(hwnd, &client_width, &client_height);
        if (!status || client_width != width || client_height != height) {
            int released = ReleaseDC(window, dc);
            return status && released ? -1 : 0;
        }
    }
    BITMAPINFO information = {0};
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = (LONG)width;
    information.bmiHeader.biHeight = -(LONG)height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    int copied = StretchDIBits(dc, 0, 0, (int)client_width, (int)client_height,
                              0, 0, (int)width, (int)height, pixels, &information,
                              DIB_RGB_COLORS, SRCCOPY);
    int released = ReleaseDC(window, dc);
    if (copied != (int)height || !released)
        return 0;
    return present_mode != 2 || SUCCEEDED(DwmFlush());
#else
    (void)hwnd;
    (void)exact;
    return 0;
#endif
}

int venus_win32_present_pixels(uintptr_t hwnd, uint32_t width, uint32_t height,
                               const void *pixels, size_t bytes, uint32_t present_mode) {
    return present_pixels(hwnd, width, height, pixels, bytes, present_mode, 0);
}
int venus_win32_present_pixels_exact(uintptr_t hwnd, uint32_t width, uint32_t height,
                                     const void *pixels, size_t bytes, uint32_t present_mode) {
    return present_pixels(hwnd, width, height, pixels, bytes, present_mode, 1);
}
