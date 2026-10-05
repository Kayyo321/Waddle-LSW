/** @file av_wgc.cpp @brief C++ compatibility boundary for system C++/WinRT SDK.
 * OS SDK-generated WinRT interfaces require C++; all transport stays C/Zig.
 */
#include "av_wgc.h"
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

using capture_item_t = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using capture_pool_t = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using capture_session_t = winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using direct_device_t = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using pixel_format_t = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using size_t_t = winrt::Windows::Graphics::SizeInt32;
/** @brief RAII instance; one creating thread owns every device/frame/session.
 * COM smart pointers own references; destructor closes WinRT objects before
 * releasing device and balancing its RoInitialize call. No raw new/delete.
 */
struct av_wgc_t {
    bool apartment_initialized = false;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> device_context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    direct_device_t winrt_device{nullptr};
    capture_item_t item{nullptr};
    capture_pool_t pool{nullptr};
    capture_session_t session{nullptr};
    size_t_t size{};
    ~av_wgc_t() noexcept {
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr; pool = nullptr; item = nullptr; winrt_device = nullptr;
        staging = nullptr; device_context = nullptr; device = nullptr;
        if (apartment_initialized) RoUninitialize();
    }
};
/** @brief Scoped CPU texture mapping; destructor unmaps before releasing references. */
struct mapped_texture_t {
    winrt::com_ptr<ID3D11DeviceContext> device_context;
    winrt::com_ptr<ID3D11Texture2D> texture;
    bool mapped = false;
    ~mapped_texture_t() noexcept { if (mapped) device_context->Unmap(texture.get(), 0); }
};
extern "C" __declspec(dllexport) HRESULT av_wgc_create(HWND window, av_wgc_t **result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!window || !IsWindow(window)) return E_INVALIDARG;
    try {
        auto capture = std::make_unique<av_wgc_t>();
        winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED));
        capture->apartment_initialized = true;
        if (!capture_session_t::IsSupported()) return E_NOTIMPL;
        HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            capture->device.put(), nullptr, capture->device_context.put());
        if (FAILED(status)) {
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                capture->device.put(), nullptr, capture->device_context.put()));
        }
        auto dxgi_device = capture->device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
        capture->winrt_device = inspectable.as<direct_device_t>();
        auto factory = winrt::get_activation_factory<capture_item_t, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(window, winrt::guid_of<capture_item_t>(),
            winrt::put_abi(capture->item)));
        capture->size = capture->item.Size();
        if (capture->size.Width <= 0 || capture->size.Height <= 0) return E_INVALIDARG;
        capture->pool = capture_pool_t::CreateFreeThreaded(capture->winrt_device,
            pixel_format_t::B8G8R8A8UIntNormalized, 3, capture->size);
        capture->session = capture->pool.CreateCaptureSession(capture->item);
        capture->session.IsCursorCaptureEnabled(false);
        capture->session.StartCapture();
        *result = capture.release();
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" __declspec(dllexport) HRESULT av_wgc_read(av_wgc_t *capture, av_wgc_copy_t copy, void *context) {
    if (!capture || !copy) return E_POINTER;
    try {
        auto frame = capture->pool.TryGetNextFrame();
        if (!frame) return S_FALSE;
        struct frame_close_t {
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame;
            ~frame_close_t() noexcept { try { if (frame) frame.Close(); } catch (...) {} }
        } frame_lifetime{frame};
        auto content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0 || content.Width > 8192 || content.Height > 8192) return E_INVALIDARG;
        if (content.Width != capture->size.Width || content.Height != capture->size.Height) {
            frame.Close();
            frame_lifetime.frame = nullptr;
            capture->pool.Recreate(capture->winrt_device, pixel_format_t::B8G8R8A8UIntNormalized, 3, content);
            capture->size = content;
            capture->staging = nullptr;
            return S_FALSE;
        }
        auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            description.Width < static_cast<UINT>(content.Width) || description.Height < static_cast<UINT>(content.Height)) return E_INVALIDARG;
        if (!capture->staging) {
            description.Width = static_cast<UINT>(content.Width);
            description.Height = static_cast<UINT>(content.Height);
            description.MipLevels = 1; description.ArraySize = 1;
            description.SampleDesc = {1, 0}; description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ; description.MiscFlags = 0;
            winrt::check_hresult(capture->device->CreateTexture2D(&description, nullptr, capture->staging.put()));
        }
        D3D11_BOX box{0, 0, 0, static_cast<UINT>(content.Width), static_cast<UINT>(content.Height), 1};
        capture->device_context->CopySubresourceRegion(capture->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
        mapped_texture_t mapping{capture->device_context, capture->staging};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(capture->device_context->Map(capture->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        mapping.mapped = true;
        auto timestamp = frame.SystemRelativeTime().count();
        return copy(static_cast<const uint8_t *>(mapped.pData),
            static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(content.Height), mapped.RowPitch,
            static_cast<uint32_t>(content.Width), static_cast<uint32_t>(content.Height),
            timestamp > 0 ? static_cast<uint64_t>(timestamp) * 100 : 0, context);
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" __declspec(dllexport) void av_wgc_destroy(av_wgc_t **capture) {
    if (!capture) return;
    std::unique_ptr<av_wgc_t> owned(*capture);
    *capture = nullptr;
}
