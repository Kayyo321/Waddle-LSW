#ifndef WaddleAvWgcH
#define WaddleAvWgcH
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Opaque WinRT capture instance, exclusively owned by one capture thread. */
typedef struct av_wgc_t av_wgc_t;
/** @brief Borrowed mapped-texture callback, no ownership transfer.
 * @param[in] pixels Nonnull GPU mapping, valid only during callback.
 * @param[in] length Accessible mapped row bytes.
 * @param[in] stride GPU bytes per row. @param[in] width Nonzero pixel width.
 * @param[in] height Nonzero pixel height. @param[in] timestamp_ns Guest capture time.
 * @param[in,out] context Optional borrowed context.
 * @return S_OK copied/published, failing HRESULT rejects the frame.
 * @note Capture thread only; callback must not retain pixels or throw exceptions.
 */
typedef HRESULT (*av_wgc_copy_t)(const uint8_t *pixels, size_t length, uint32_t stride,
                                 uint32_t width, uint32_t height, uint64_t timestamp_ns, void *context);
/** @brief Create per-window WinRT capture including occluded/non-client content.
 * @param[in] window Nonnull valid borrowed HWND.
 * @param[out] capture Nonnull caller-owned opaque result; NULL on failure.
 * @return S_OK or HRESULT, no partially owned instance escapes.
 * @note Caller takes ownership until destroy; one MTA capture thread per instance.
 */
HRESULT av_wgc_create(HWND window, av_wgc_t **capture);
/** @brief Poll one per-window capture frame and invoke a bounded C copy callback.
 * @param[in,out] capture Nonnull initialized owned instance.
 * @param[in] copy Nonnull callback, not retained. @param[in,out] context Borrowed context.
 * @return S_OK delivered, S_FALSE no frame/resize, or failing HRESULT.
 * @note Capture thread only; no raw ownership escapes; mapped texture is unmapped
 * and WinRT frame closed on every path before return.
 */
HRESULT av_wgc_read(av_wgc_t *capture, av_wgc_copy_t copy, void *context);
/** @brief Close capture session/pool and release opaque instance.
 * @param[in,out] capture Nonnull pointer to owned instance; NULL value accepted.
 * @note Creating capture thread only, idempotent; sets *capture NULL; no result.
 */
void av_wgc_destroy(av_wgc_t **capture);
/** @brief C ABI procedure types used by the loader; DLL retained until destroy. */
typedef HRESULT (*av_wgc_create_t)(HWND window, av_wgc_t **capture);
/** @brief C ABI frame procedure type; pointers borrowed for each call. */
typedef HRESULT (*av_wgc_read_t)(av_wgc_t *capture, av_wgc_copy_t copy, void *context);
/** @brief C ABI destructor procedure type; owned capture released and NULLed. */
typedef void (*av_wgc_destroy_t)(av_wgc_t **capture);
#ifdef __cplusplus
}
#endif
#endif
