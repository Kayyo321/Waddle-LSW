#define COBJMACROS
#include "av_capture.h"
#include <string.h>

void av_capture_free(av_capture_t *capture) {
    if (capture->staging)
        ID3D11Texture2D_Release(capture->staging);
    if (capture->duplication)
        IDXGIOutputDuplication_Release(capture->duplication);
    if (capture->context)
        ID3D11DeviceContext_Release(capture->context);
    if (capture->device)
        ID3D11Device_Release(capture->device);
    memset(capture, 0, sizeof(*capture));
}
HRESULT av_capture_init(av_capture_t *capture, HWND window) {
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput *output = NULL;
    IDXGIOutput1 *output_one = NULL;
    HRESULT result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    HMONITOR target = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (FAILED(result))
        goto cleanup;
    result = DXGI_ERROR_NOT_FOUND;
    for (UINT adapter_index = 0;; ++adapter_index) {
        if (IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter) != S_OK)
            break;
        for (UINT output_index = 0;; ++output_index) {
            if (IDXGIAdapter1_EnumOutputs(adapter, output_index, &output) != S_OK)
                break;
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(IDXGIOutput_GetDesc(output, &desc)) && desc.Monitor == target) {
                capture->desktop_bounds = desc.DesktopCoordinates;
                result =
                    D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
                                      &capture->device, NULL, &capture->context);
                if (FAILED(result))
                    goto cleanup;
                result =
                    IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output_one);
                if (FAILED(result))
                    goto cleanup;
                result = IDXGIOutput1_DuplicateOutput(output_one, (IUnknown *)capture->device,
                                                      &capture->duplication);
                goto cleanup;
            }
            IDXGIOutput_Release(output);
            output = NULL;
        }
        IDXGIAdapter1_Release(adapter);
        adapter = NULL;
    }
cleanup:
    if (output_one)
        IDXGIOutput1_Release(output_one);
    if (output)
        IDXGIOutput_Release(output);
    if (adapter)
        IDXGIAdapter1_Release(adapter);
    if (factory)
        IDXGIFactory1_Release(factory);
    if (FAILED(result))
        av_capture_free(capture);
    return result;
}
HRESULT av_capture_frame(av_capture_t *capture, const RECT *bounds, window_slot_header_t *slot,
                         uint8_t *pixels, size_t capacity) {
    IDXGIResource *resource = NULL;
    ID3D11Texture2D *texture = NULL;
    DXGI_OUTDUPL_FRAME_INFO info;
    D3D11_MAPPED_SUBRESOURCE mapped;
    int frame_owned = 0, mapped_owned = 0;
    HRESULT result;
    int64_t wide = (int64_t)bounds->right - bounds->left;
    int64_t high = (int64_t)bounds->bottom - bounds->top;
    if (wide <= 0 || high <= 0 || wide > 8192 || high > 8192 ||
        bounds->left < capture->desktop_bounds.left || bounds->top < capture->desktop_bounds.top ||
        bounds->right > capture->desktop_bounds.right ||
        bounds->bottom > capture->desktop_bounds.bottom)
        return E_INVALIDARG;
    uint32_t width = (uint32_t)wide, height = (uint32_t)high;
    if (!av_video_size(width, height, width * 4, capacity))
        return E_INVALIDARG;
    if (!av_video_begin_write(slot))
        return S_FALSE;
    result = IDXGIOutputDuplication_AcquireNextFrame(capture->duplication, 0, &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        av_video_cancel(slot);
        return S_FALSE;
    }
    if (FAILED(result))
        goto cleanup;
    frame_owned = 1;
    result = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture);
    if (FAILED(result))
        goto cleanup;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1) {
        result = E_NOTIMPL;
        goto cleanup;
    }
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    if (capture->staging) {
        D3D11_TEXTURE2D_DESC old;
        ID3D11Texture2D_GetDesc(capture->staging, &old);
        if (old.Width != width || old.Height != height) {
            ID3D11Texture2D_Release(capture->staging);
            capture->staging = NULL;
        }
    }
    if (!capture->staging) {
        result =
            ID3D11Device_CreateTexture2D(capture->device, &staging_desc, NULL, &capture->staging);
        if (FAILED(result))
            goto cleanup;
    }
    D3D11_BOX crop = {.left = (UINT)(bounds->left - capture->desktop_bounds.left),
                      .top = (UINT)(bounds->top - capture->desktop_bounds.top),
                      .front = 0,
                      .right = (UINT)(bounds->right - capture->desktop_bounds.left),
                      .bottom = (UINT)(bounds->bottom - capture->desktop_bounds.top),
                      .back = 1};
    if (crop.right > desc.Width || crop.bottom > desc.Height) {
        result = E_INVALIDARG;
        goto cleanup;
    }
    ID3D11DeviceContext_CopySubresourceRegion(capture->context, (ID3D11Resource *)capture->staging,
                                              0, 0, 0, 0, (ID3D11Resource *)texture, 0, &crop);
    result = ID3D11DeviceContext_Map(capture->context, (ID3D11Resource *)capture->staging, 0,
                                     D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
        goto cleanup;
    mapped_owned = 1;
    if (av_copy_bgra(mapped.pData, (size_t)mapped.RowPitch * height, mapped.RowPitch, pixels,
                     capacity, width, height) != 0) {
        result = E_INVALIDARG;
        goto cleanup;
    }
    slot->width = width;
    slot->height = height;
    slot->stride = width * 4;
    slot->format = AvPixelFormat;
    slot->frame_sequence = ++capture->sequence;
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    slot->timestamp_ns = (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000000u +
                         (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000000u /
                             (uint64_t)frequency.QuadPart;
    result = av_video_publish(slot) ? S_OK : E_FAIL;
cleanup:
    if (mapped_owned)
        ID3D11DeviceContext_Unmap(capture->context, (ID3D11Resource *)capture->staging, 0);
    if (texture)
        ID3D11Texture2D_Release(texture);
    if (resource)
        IDXGIResource_Release(resource);
    if (frame_owned)
        IDXGIOutputDuplication_ReleaseFrame(capture->duplication);
    if (FAILED(result))
        av_video_cancel(slot);
    return result;
}
