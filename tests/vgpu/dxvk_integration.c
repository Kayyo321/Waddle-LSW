/** @file dxvk_integration.c @brief Native black-box DXVK device/presentation gate.
 * All SDK identifiers below are external ABI names. This fixture owns every
 * acquired COM reference, window and module until the single cleanup path.
 * Build success alone does not establish ICD or DXVK compatibility.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <wchar.h>

/** @brief Borrowed DXVK export signature; invocation transfers output references. */
typedef HRESULT (WINAPI *create_device_swapchain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
    D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* Accept ordinary absolute drive/UNC paths; refuse search-path resolution. */
static int absolute_file(const wchar_t *path, const wchar_t *basename) {
    if (!path || !*path)
        return 0;
    size_t length = wcslen(path);
    if (!((length >= 3 && path[1] == L':' && path[2] == L'\\') ||
          (length >= 3 && path[0] == L'\\' && path[1] == L'\\')))
        return 0;
    const wchar_t *leaf = wcsrchr(path, L'\\');
    if (basename && (!leaf || _wcsicmp(leaf + 1, basename)))
        return 0;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/** @brief Run one real DXVK device/swapchain transaction on the selected ICD.
 * @param[in] argc Exactly five. @param[in] argv Nonnull borrowed absolute paths:
 * executable, vulkan-1.dll, DXVK dxgi.dll, DXVK d3d11.dll, Waddle ICD manifest.
 * @return Zero after successful device, clear, Present and teardown; two for
 * invalid prerequisites; one for native/DXVK failure. No skipped success.
 * @note Single main thread; SDK/DXVK may own internal threads. No fixture heap.
 * Modules outlive COM objects; partial acquisition uses the same cleanup path.
 */
int wmain(int argc, wchar_t **argv) {
    if (argc != 5 || !absolute_file(argv[1], L"vulkan-1.dll") ||
        !absolute_file(argv[2], L"dxgi.dll") || !absolute_file(argv[3], L"d3d11.dll") ||
        !absolute_file(argv[4], NULL) || wcschr(argv[4], L';')) {
        fputs("usage: dxvk_integration.exe <absolute vulkan-1.dll> <absolute DXVK dxgi.dll> "
              "<absolute DXVK d3d11.dll> <absolute Waddle ICD manifest>\n", stderr);
        return 2;
    }
    /* Replace loader discovery, rather than adding the experimental driver to
     * the installed GPU list. The manifest is a trusted acceptance input. */
    if (!SetEnvironmentVariableW(L"VK_DRIVER_FILES", argv[4]) ||
        !SetEnvironmentVariableW(L"VK_ICD_FILENAMES", NULL) ||
        !SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", NULL) ||
        !SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~all~")) {
        fprintf(stderr, "loader environment failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    int result = 1;
    HMODULE loader = NULL, dxgi = NULL, d3d11 = NULL;
    HWND window = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *backbuffer = NULL;
    ID3D11RenderTargetView *view = NULL;
    ID3D11Query *completion = NULL;
    HRESULT status = E_FAIL;
    loader = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader)
        goto cleanup;
    dxgi = LoadLibraryExW(argv[2], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dxgi)
        goto cleanup;
    d3d11 = LoadLibraryExW(argv[3], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!d3d11)
        goto cleanup;
    create_device_swapchain_t create =
        (create_device_swapchain_t)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create)
        goto cleanup;
    window = CreateWindowExW(0, L"STATIC", L"Waddle DXVK acceptance",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             0, 0, 64, 64, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!window)
        goto cleanup;
    DXGI_SWAP_CHAIN_DESC description = {0};
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected = 0;
    status = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    Levels, 1, D3D11_SDK_VERSION, &description, &swapchain, &device,
                    &selected, &context);
    if (FAILED(status) || !swapchain || !device || !context || selected != Levels[0])
        goto cleanup;
    status = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    if (FAILED(status) || !backbuffer)
        goto cleanup;
    status = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)backbuffer, NULL, &view);
    if (FAILED(status) || !view)
        goto cleanup;
    D3D11_QUERY_DESC query_description = {.Query = D3D11_QUERY_EVENT};
    status = ID3D11Device_CreateQuery(device, &query_description, &completion);
    if (FAILED(status) || !completion)
        goto cleanup;
    const FLOAT Color[] = {0.125f, 0.25f, 0.5f, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(context, view, Color);
    ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)completion);
    ID3D11DeviceContext_Flush(context);
    ULONGLONG started = GetTickCount64();
    BOOL finished = FALSE;
    do {
        status = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)completion,
                                            &finished, sizeof(finished), 0);
        if (FAILED(status))
            goto cleanup;
        if (status == S_OK && finished)
            break;
        if (GetTickCount64() - started >= 10000) {
            status = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            goto cleanup;
        }
        Sleep(1);
    } while (1);
    status = IDXGISwapChain_Present(swapchain, 0, 0);
    if (status != S_OK)
        goto cleanup;
    status = ID3D11Device_GetDeviceRemovedReason(device);
    if (FAILED(status))
        goto cleanup;
    result = 0;
cleanup:
    if (result)
        fprintf(stderr, "DXVK acceptance failed: HRESULT=0x%08lx Win32=%lu\n",
                (unsigned long)status, (unsigned long)GetLastError());
    if (context)
        ID3D11DeviceContext_ClearState(context);
    if (completion)
        ID3D11Query_Release(completion);
    if (view)
        ID3D11RenderTargetView_Release(view);
    if (backbuffer)
        ID3D11Texture2D_Release(backbuffer);
    if (swapchain)
        IDXGISwapChain_Release(swapchain);
    if (context)
        ID3D11DeviceContext_Release(context);
    if (device)
        ID3D11Device_Release(device);
    if (window && !DestroyWindow(window))
        result = 1;
    if (d3d11 && !FreeLibrary(d3d11))
        result = 1;
    if (dxgi && !FreeLibrary(dxgi))
        result = 1;
    if (loader && !FreeLibrary(loader))
        result = 1;
    if (!result)
        puts("DXVK device, backbuffer clear and Present succeeded on the selected ICD.");
    return result;
}
