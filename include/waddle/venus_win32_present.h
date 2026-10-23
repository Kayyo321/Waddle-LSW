/** @file venus_win32_present.h @brief Synchronous opaque Win32 compatibility presentation. */
#ifndef WaddleVenusWin32PresentH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusWin32PresentH
#include <stddef.h>
#include <stdint.h>
/** @brief Query a borrowed live window's current client extent.
 * @param[in] hwnd Borrowed native HWND bits, never retained or destroyed.
 * @param[out] width Nonnull exclusive scalar, zero on failure.
 * @param[out] height Nonnull distinct exclusive scalar, zero on failure.
 * @return One on success; zero invalid/destroyed window or unsupported OS. A valid zero-area
 * client rectangle succeeds with a zero dimension; callers must handle minimize.
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
 * during the call and always attempts ReleaseDC; native failure propagates; no persistent DC/GDI object/heap.
 */
int venus_win32_present_pixels(uintptr_t hwnd, uint32_t width, uint32_t height,
                               const void *pixels, size_t bytes, uint32_t present_mode);
/** @brief Display completed BGRA8 pixels only at their exact current client extent.
 * @param[in] hwnd Live borrowed HWND bits, retained by caller until return.
 * @param[in] width Source width 1..16384 pixels, must equal current client width.
 * @param[in] height Source height 1..16384 pixels, must equal current client height.
 * @param[in] pixels Nonnull immutable pixels[bytes], borrowed for this call only.
 * @param[in] bytes Accessible extent, at least width*height*4.
 * @param[in] present_mode Vulkan IMMEDIATE(0) or FIFO(2).
 * @return One completed presentation; negative one extent mismatch/minimize;
 * zero invalid input/window/native failure or unsupported platform.
 * @note Presentation thread only, no retained allocation. Checks extent before
 * and after GetDC, never scales pixels, and always attempts ReleaseDC for acquired DCs; native failure propagates. Caller
 * synchronizes window lifetime; snapshots cannot freeze concurrent OS events.
 */
int venus_win32_present_pixels_exact(uintptr_t hwnd, uint32_t width, uint32_t height,
                                     const void *pixels, size_t bytes, uint32_t present_mode);
#endif
