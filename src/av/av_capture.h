#ifndef WaddleAvCaptureH
#define WaddleAvCaptureH
#define COBJMACROS
#include "av_transport.h"
#include "av_wgc.h"
#include <d3d11.h>
#include <dxgi1_2.h>
/** @brief Caller-owned DXGI capture resources, one capture thread per instance.
 * @note Initialize to zero. init owns COM references; free releases and NULLs
 * every reference. No heap allocations owned by caller, no shared access.
 */
typedef struct av_capture_t {
    HMODULE wgc_library;          /**< Owned sibling WGC DLL, retained until opaque destroy. */
    av_wgc_t *wgc;                /**< Owned per-window WinRT context. */
    av_wgc_read_t wgc_read;       /**< Borrowed DLL procedure while library is loaded. */
    av_wgc_destroy_t wgc_destroy; /**< Borrowed DLL destructor procedure. */
    ID3D11Device *device;         /**< Owned device until free. */
    ID3D11DeviceContext *context; /**< Owned immediate context until free. */
    IDXGIOutputDuplication *duplication; /**< Owned duplication until free. */
    ID3D11Texture2D *staging;            /**< Owned reusable CPU-readable staging texture. */
    RECT desktop_bounds;                 /**< Output's guest virtual-screen bounds. */
    uint64_t sequence;                   /**< Last published sequence. */
} av_capture_t;
/** @brief Initialize Desktop Duplication on the monitor containing a target HWND.
 * @param[out] capture Nonnull zero-initialized caller-owned instance.
 * @param[in] window Nonnull valid target window; borrowed during call.
 * @return S_OK on success, HRESULT on failure; partial resources released.
 * @note Capture thread only; COM must already be initialized. Visible-window
 * cropping does not preserve occluded content or original transparent alpha.
 */
HRESULT av_capture_init(av_capture_t *capture, HWND window);
/** @brief Capture a visible window crop into a producer-owned bounded video slot.
 * @param[in,out] capture Nonnull initialized owned context.
 * @param[in] bounds Nonnull borrowed DWM window bounds in guest screen pixels.
 * @param[in,out] slot Nonnull caller-owned aligned header; only Free is claimed.
 * @param[out] pixels Nonnull borrowed payload of capacity accessible bytes.
 * @param[in] capacity Accessible bytes, mapping remains alive until host release.
 * @return S_OK on publication, S_FALSE if no frame/busy, HRESULT on loss/error.
 * @note Capture thread only; never overwrites Ready/Consuming, bounded Zig copy.
 */
HRESULT av_capture_frame(av_capture_t *capture, const RECT *bounds, window_slot_header_t *slot,
                         uint8_t *pixels, size_t capacity);
/** @brief Release all owned COM resources and zero the caller-owned instance.
 * @param[in,out] capture Nonnull instance; may be partially initialized/empty.
 * @note Capture thread only, idempotent; no return value/ownership transfer.
 */
void av_capture_free(av_capture_t *capture);
/** @brief Copy bounded rows of BGRA data from a mapped GPU texture.
 * @param[in] source Nonnull borrowed source[source_len].
 * @param[in] source_len Accessible source bytes including row padding.
 * @param[in] source_stride Source row bytes, at least width * 4.
 * @param[out] output Nonnull nonoverlapping output[output_len], caller owned.
 * @param[in] output_len Accessible destination bytes.
 * @param[in] width Nonzero crop width in pixels.
 * @param[in] height Nonzero crop height in pixels.
 * @return 0 if copied, -1 on invalid bounds with output unchanged.
 * @note Pure/thread-safe, no allocation; requires caller-owned Writing slot.
 */
int av_copy_bgra(const uint8_t *source, size_t source_len, uint32_t source_stride, uint8_t *output,
                 size_t output_len, uint32_t width, uint32_t height);
#endif
