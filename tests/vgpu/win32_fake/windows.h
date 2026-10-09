/** @file windows.h @brief Test-only API declarations, not Windows SDK or runtime code.
 * @note Identifiers intentionally match the external Win32 ABI used by the sink.
 * All handles are borrowed fixture tokens. No declarations are installed/shipped.
 */
#ifndef WaddleTestWin32H
/** @brief Test include guard; no state. */
#define WaddleTestWin32H
#include <stdint.h>
/** @brief Borrowed opaque fixture window token. */
typedef void *HWND;
/** @brief Borrowed opaque fixture drawing context token. */
typedef void *HDC;
/** @brief Exact Win32 signed scalar. */
typedef int32_t LONG;
/** @brief Native client rectangle fields consumed by production. */
typedef struct { LONG left, top, right, bottom; } RECT;
/** @brief Native bitmap header fields consumed by production. */
typedef struct {
    uint32_t biSize;
    LONG biWidth, biHeight;
    uint16_t biPlanes, biBitCount;
    uint32_t biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    uint32_t biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
/** @brief Native bitmap metadata; fixture never interprets a color table. */
typedef struct { BITMAPINFOHEADER bmiHeader; uint32_t bmiColors[1]; } BITMAPINFO;
/** @brief Win32 uncompressed bitmap tag. */
#define BI_RGB 0
/** @brief Win32 RGB color mode. */
#define DIB_RGB_COLORS 0
/** @brief Win32 source-copy raster operation. */
#define SRCCOPY 0x00cc0020
/** @brief Borrowed window predicate; single-thread fixture, no ownership change. */
int IsWindow(HWND window);
/** @brief Write client rectangle on success; borrowed window and exclusive output. */
int GetClientRect(HWND window, RECT *bounds);
/** @brief Acquire one fixture DC owner or return NULL on injected failure. */
HDC GetDC(HWND window);
/** @brief Release borrowed fixture DC once; return injected native status. */
int ReleaseDC(HWND window, HDC dc);
/** @brief Validate borrowed pixel/metadata arguments; return copied scanline count. */
int StretchDIBits(HDC dc, int x, int y, int width, int height, int source_x, int source_y,
                 int source_width, int source_height, const void *pixels,
                 const BITMAPINFO *information, unsigned usage, unsigned operation);
#endif
