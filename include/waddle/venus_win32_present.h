/** @file venus_win32_present.h @brief Synchronous opaque Win32 compatibility presentation. */
#ifndef WaddleVenusWin32PresentH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusWin32PresentH
#include <stddef.h>
#include <stdint.h>
/** @brief Query a borrowed live window's nonempty client extent.
 * @param[in] hwnd Borrowed native HWND bits, never retained or destroyed.
 * @param[out] width Nonnull exclusive scalar, zero on failure.
 * @param[out] height Nonnull distinct exclusive scalar, zero on failure.
 * @return One on success; zero invalid/destroyed/empty window or unsupported OS.
 * @note Calling presentation thread only; allocation-free, no retained resources.
 */
int venus_win32_present_extent(uintptr_t hwnd, uint32_t *width, uint32_t *height);
/** @brief Display completed tightly packed BGRA8 pixels in a borrowed window.
 * @param[in] hwnd Live borrowed HWND bits, never retained or destroyed.
 * @param[in] width Source width1..16384 pixels.
 * @param[in] height Source height1..16384 pixels.
 * @param[in] pixels Nonnull accessible immutable pixels[bytes], borrowed until return.
 * @param[in] bytes Actual accessible extent, at least width*height*4.
 * @param[in] present_mode Vulkan IMMEDIATE(0) or FIFO(2), paced with DwmFlush.
 * @return One complete presentation, zero invalid input/window/native failure or unsupported OS.
 * @note Presentation thread only. Caller completes GPU readback before calling.
 * Top-down opaque BGRA8 is scaled to current client extent. Owns GetDC only
 * during the call and always releases it; no persistent DC/GDI object/heap.
 */
int venus_win32_present_pixels(uintptr_t hwnd, uint32_t width, uint32_t height,
                               const void *pixels, size_t bytes, uint32_t present_mode);
#endif
