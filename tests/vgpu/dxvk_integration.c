/** @file dxvk_integration.c @brief Native black-box DXVK device/presentation gate.
 * All SDK identifiers below are external ABI names. This fixture owns every
 * acquired COM reference, window and module until the single cleanup path.
 * Build success alone does not establish ICD or DXVK compatibility.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include "waddle/venus_tcp.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

/** @brief Borrowed DXVK export signature; invocation transfers output references. */
typedef HRESULT (WINAPI *create_device_swapchain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
    D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/** @brief System compiler export; returned blobs belong to the caller. */
typedef HRESULT (WINAPI *compile_shader_t)(const void *,SIZE_T,const char *,const D3D_SHADER_MACRO *,ID3DInclude *,const char *,const char *,UINT,UINT,ID3DBlob **,ID3DBlob **);
/** @brief Explicit bootstrap start; path borrowed for call, session owned by DLL. */
typedef venus_ring_status_t (*bootstrap_start_t)(const char *,size_t);
/** @brief Quiescent stop; failed retirement retains the DLL session. */
typedef venus_ring_status_t (*bootstrap_stop_t)(void);
/** @brief Borrowed copied session identity; no resource transfer. */
typedef uint64_t (*bootstrap_session_t)(void);
/** @brief Release only after exact externally proved worker retirement. */
typedef venus_ring_status_t (*bootstrap_abandon_t)(uint64_t);

/** @brief Verify every BGRA pixel through actual GPU-to-staging copy.
 * @param[in] device/context/backbuffer Live borrowed COM references.
 * @return S_OK for 4096 exact pixels, failure HRESULT otherwise.
 * @note Single caller thread. Local staging owner released on every path;
 * mapping is borrowed from context until Unmap and never escapes this call.
 */
static HRESULT verify_pixels(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *backbuffer) {
    D3D11_TEXTURE2D_DESC description={0};ID3D11Texture2D_GetDesc(backbuffer,&description);
    if(description.Width!=64 || description.Height!=64 || description.Format!=DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count!=1)return E_FAIL;
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;
    ID3D11Texture2D *staging=NULL;HRESULT status=ID3D11Device_CreateTexture2D(device,&description,NULL,&staging);
    if(FAILED(status))return status;
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)backbuffer);
    D3D11_MAPPED_SUBRESOURCE mapping={0};status=ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapping);
    if(SUCCEEDED(status)) {
        if(!mapping.pData || mapping.RowPitch<256)status=E_FAIL;
        else for(UINT row=0;row<64;row++)for(UINT column=0;column<64;column++) {
            const unsigned char *pixel=(const unsigned char *)mapping.pData+(size_t)row*mapping.RowPitch+column*4;
            if(pixel[0]!=128 || pixel[1]!=64 || pixel[2]!=32 || pixel[3]!=255)status=E_FAIL;
        }
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
    }
    ID3D11Texture2D_Release(staging);
    if(SUCCEEDED(status))puts("DXVK rendered 4096 exact BGRA pixels.");
    return status;
}

/** @brief Verify the actual presented client area after compositor completion.
 * @param[in] window Live borrowed 64 by 64 client HWND.
 * @return S_OK for every exact presented RGB pixel; failure otherwise.
 * @note Sole caller thread. Owns its acquired DC until unconditional ReleaseDC.
 */
static HRESULT verify_presented_pixels(HWND window) {
    if(DwmFlush()!=S_OK)return E_FAIL;
    RECT client={0};
    if(!GetClientRect(window,&client) || client.right!=64 || client.bottom!=64)return E_FAIL;
    HDC dc=GetDC(window);if(!dc)return E_FAIL;
    HRESULT status=S_OK;
    for(int row=0;row<64;row++)for(int column=0;column<64;column++)
        if(GetPixel(dc,column,row)!=RGB(32,64,128))status=E_FAIL;
    if(!ReleaseDC(window,dc))status=E_FAIL;
    if(SUCCEEDED(status))puts("DXVK presented 4096 exact client RGB pixels.");
    return status;
}

/** @brief Execute real DXVK compute and verify all 64 storage-buffer words.
 * @param[in] device/context Live borrowed COM references, held by caller.
 * @return S_OK after exact results, failure HRESULT otherwise.
 * @note Single caller thread; local COM/module owners released at cleanup.
 * Compiler blobs own bytecode only until CreateComputeShader copies it.
 */
static HRESULT verify_compute(ID3D11Device *device,ID3D11DeviceContext *context) {
    static const char Shader[]="RWStructuredBuffer<uint> data : register(u0); [numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) { data[id.x]=id.x*3+7; }";
    HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HRESULT status=E_FAIL;ID3DBlob *code=NULL,*errors=NULL;ID3D11ComputeShader *shader=NULL;
    ID3D11Buffer *output=NULL,*staging=NULL;ID3D11UnorderedAccessView *view=NULL;
    if(!compiler)goto cleanup;
    FARPROC procedure=GetProcAddress(compiler,"D3DCompile");compile_shader_t compile=NULL;
    _Static_assert(sizeof compile==sizeof procedure,"Windows function representation");memcpy(&compile,&procedure,sizeof compile);
    if(!compile)goto cleanup;
    status=compile(Shader,sizeof Shader-1,"acceptance_compute",NULL,NULL,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(status) || !code)goto cleanup;
    status=ID3D11Device_CreateComputeShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&shader);
    if(FAILED(status))goto cleanup;
    D3D11_BUFFER_DESC description={.ByteWidth=256,.Usage=D3D11_USAGE_DEFAULT,.BindFlags=D3D11_BIND_UNORDERED_ACCESS,.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,.StructureByteStride=4};
    status=ID3D11Device_CreateBuffer(device,&description,NULL,&output);if(FAILED(status))goto cleanup;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_description={.Format=DXGI_FORMAT_UNKNOWN,.ViewDimension=D3D11_UAV_DIMENSION_BUFFER};view_description.Buffer.NumElements=64;
    status=ID3D11Device_CreateUnorderedAccessView(device,(ID3D11Resource *)output,&view_description,&view);if(FAILED(status))goto cleanup;
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;description.StructureByteStride=0;
    status=ID3D11Device_CreateBuffer(device,&description,NULL,&staging);if(FAILED(status))goto cleanup;
    ID3D11DeviceContext_CSSetShader(context,shader,NULL,0);ID3D11DeviceContext_CSSetUnorderedAccessViews(context,0,1,&view,NULL);
    ID3D11DeviceContext_Dispatch(context,8,1,1);
    ID3D11UnorderedAccessView *empty=NULL;ID3D11DeviceContext_CSSetUnorderedAccessViews(context,0,1,&empty,NULL);ID3D11DeviceContext_CSSetShader(context,NULL,NULL,0);
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)output);
    D3D11_MAPPED_SUBRESOURCE mapping={0};status=ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapping);
    if(SUCCEEDED(status)) {
        if(!mapping.pData)status=E_FAIL;
        else for(UINT index=0;index<64;index++){UINT value=0;memcpy(&value,(const unsigned char *)mapping.pData+index*4,4);if(value!=index*3+7)status=E_FAIL;}
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
    }
cleanup:
    ID3D11DeviceContext_CSSetShader(context,NULL,NULL,0);
    ID3D11UnorderedAccessView *none=NULL;ID3D11DeviceContext_CSSetUnorderedAccessViews(context,0,1,&none,NULL);
    if(view)ID3D11UnorderedAccessView_Release(view);
    if(staging)ID3D11Buffer_Release(staging);
    if(output)ID3D11Buffer_Release(output);
    if(shader)ID3D11ComputeShader_Release(shader);
    if(errors)ID3D10Blob_Release(errors);
    if(code)ID3D10Blob_Release(code);
    if(compiler && !FreeLibrary(compiler))status=E_FAIL;
    if(SUCCEEDED(status))puts("DXVK compute returned all 64 exact storage words.");
    return status;
}

/** @brief Wait for the supervisor's exact session retirement record.
 * @param[in] path Trusted fresh absolute UTF-16 receipt path, borrowed.
 * @param[in] identity Nonzero retained session identity.
 * @note Failure retains ownership until the supervisor proves host retirement.
 * Sole caller thread; each transient receipt handle is closed before retry.
 */
static void wait_retired(const wchar_t *path,uint64_t identity) {
    char expected[64];int length=snprintf(expected,sizeof expected,"retired_session=%016llx\n",(unsigned long long)identity);
    if(length<=0 || (size_t)length>=sizeof expected)abort();
    fprintf(stderr,"Retained native session=%016llx awaiting trusted actual worker retirement\n",(unsigned long long)identity);fflush(stderr);
    for(;;) {
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information={0};char actual[65];DWORD count=0;int match=0;
            if(GetFileType(file)==FILE_TYPE_DISK && GetFileInformationByHandle(file,&information) &&
                !(information.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && information.nNumberOfLinks==1 &&
                !information.nFileSizeHigh && information.nFileSizeLow==(DWORD)length &&
                ReadFile(file,actual,sizeof actual,&count,NULL) && count==(DWORD)length && !memcmp(actual,expected,(size_t)length))match=1;
            while(!CloseHandle(file))Sleep(10);
            if(match)return;
        }
        Sleep(10);
    }
}

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
 * @param[in] argc Exactly eight. @param[in] argv Nonnull borrowed absolute paths:
 * executable, vulkan-1.dll, DXVK dxgi.dll, DXVK d3d11.dll, Waddle ICD manifest, bootstrap DLL, private config, fresh retirement receipt.
 * @return Zero after device, exact render/compute results, Present and teardown; two for
 * invalid prerequisites; one for native/DXVK failure. No skipped success.
 * @note Single main thread; SDK/DXVK may own internal threads. No fixture heap.
 * Modules outlive COM objects; partial acquisition uses the same cleanup path.
 */
int wmain(int argc, wchar_t **argv) {
    if (argc != 8 || !absolute_file(argv[1], L"vulkan-1.dll") ||
        !absolute_file(argv[2], L"dxgi.dll") || !absolute_file(argv[3], L"d3d11.dll") ||
        !absolute_file(argv[4], NULL) || wcschr(argv[4], L';') ||
        !absolute_file(argv[5], L"waddle_tcp_bootstrap.dll") || !absolute_file(argv[6], NULL) ||
        !argv[7] || wcslen(argv[7])<3 || argv[7][1]!=L':' || argv[7][2]!=L'\\' ||
        GetFileAttributesW(argv[7])!=INVALID_FILE_ATTRIBUTES) {
        fputs("usage: dxvk_integration.exe <absolute vulkan-1.dll> <absolute DXVK dxgi.dll> "
              "<absolute DXVK d3d11.dll> <absolute Waddle ICD manifest> <absolute bootstrap DLL> <private config> <fresh retirement receipt>\n", stderr);
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
    HMODULE loader = NULL, dxgi = NULL, d3d11 = NULL, bootstrap=NULL;
    bootstrap_start_t start=NULL;bootstrap_stop_t stop=NULL;bootstrap_session_t session=NULL;bootstrap_abandon_t abandon=NULL;
    uint64_t identity=0;
    HWND window = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *backbuffer = NULL;
    ID3D11RenderTargetView *view = NULL;
    ID3D11Query *completion = NULL;
    HRESULT status = E_FAIL;
    const char *stage="authenticated bootstrap";
    bootstrap=LoadLibraryExW(argv[5],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!bootstrap)goto cleanup;
    FARPROC bootstrap_proc=GetProcAddress(bootstrap,"venus_tcp_bootstrap_start");memcpy(&start,&bootstrap_proc,sizeof start);
    bootstrap_proc=GetProcAddress(bootstrap,"venus_tcp_bootstrap_stop");memcpy(&stop,&bootstrap_proc,sizeof stop);
    bootstrap_proc=GetProcAddress(bootstrap,"venus_tcp_bootstrap_session");memcpy(&session,&bootstrap_proc,sizeof session);
    bootstrap_proc=GetProcAddress(bootstrap,"venus_tcp_bootstrap_abandon");memcpy(&abandon,&bootstrap_proc,sizeof abandon);
    if(!start || !stop || !session || !abandon)goto cleanup;
    char config_path[VenusTcpMaxWindowsPathBytes];
    int path_bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[6],-1,config_path,sizeof config_path,NULL,NULL);
    if(!path_bytes || start(config_path,(size_t)path_bytes)!=RingOk)goto cleanup;
    identity=session();if(!identity)goto cleanup;
    printf("Authenticated real DXVK session=%016llx\n",(unsigned long long)identity);fflush(stdout);
    printf("DXVK actual ICD module base=%p\n",(void *)GetModuleHandleW(L"waddle_vulkan_experimental.dll"));fflush(stdout);
    stage="native loader and DXVK module acquisition";
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
                             WS_POPUP | WS_VISIBLE,
                             100, 100, 64, 64, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!window)
        goto cleanup;
    if(!UpdateWindow(window))goto cleanup;
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
    stage="real D3D11 device and swapchain creation";
    status = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    Levels, 1, D3D11_SDK_VERSION, &description, &swapchain, &device,
                    &selected, &context);
    if (FAILED(status) || !swapchain || !device || !context || selected != Levels[0])
        goto cleanup;
    puts("DXVK actual D3D11 feature level11 device and swapchain created.");fflush(stdout);
    stage="backbuffer and render target acquisition";
    status = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    if (FAILED(status) || !backbuffer)
        goto cleanup;
    status = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)backbuffer, NULL, &view);
    if (FAILED(status) || !view)
        goto cleanup;
    D3D11_QUERY_DESC query_description = {.Query = D3D11_QUERY_EVENT};
    stage="GPU completion query creation";
    status = ID3D11Device_CreateQuery(device, &query_description, &completion);
    if (FAILED(status) || !completion)
        goto cleanup;
    const FLOAT Color[] = {0.125f, 0.25f, 0.5f, 1.0f};
    stage="GPU clear and completion";
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
    stage="exact GPU staging pixels";status=verify_pixels(device,context,backbuffer);if(FAILED(status))goto cleanup;
    stage="exact compute storage words";status=verify_compute(device,context);if(FAILED(status))goto cleanup;
    stage="actual swapchain presentation";
    status = IDXGISwapChain_Present(swapchain, 0, 0);
    if (status != S_OK)
        goto cleanup;
    status=verify_presented_pixels(window);if(FAILED(status))goto cleanup;
    status = ID3D11Device_GetDeviceRemovedReason(device);
    if (FAILED(status))
        goto cleanup;
    result = 0;
cleanup:
    if (result)
        fprintf(stderr, "DXVK acceptance failed: stage=%s HRESULT=0x%08lx Win32=%lu\n",
                stage, (unsigned long)status, (unsigned long)GetLastError());
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
    if(stop && session && abandon) {
        venus_ring_status_t retirement=stop();uint64_t retained=session();
        if(retirement!=RingOk && retained){result=1;wait_retired(argv[7],retained);while(abandon(retained)!=RingOk)Sleep(10);}
        else if(retirement!=RingOk)result=1;
    }
    if (d3d11 && !FreeLibrary(d3d11))
        result = 1;
    if (dxgi && !FreeLibrary(dxgi))
        result = 1;
    if (loader && !FreeLibrary(loader))
        result = 1;
    if(bootstrap && !FreeLibrary(bootstrap))result=1;
    if(GetModuleHandleW(argv[5]) || GetModuleHandleW(argv[1]) || GetModuleHandleW(argv[2]) || GetModuleHandleW(argv[3]) || GetModuleHandleW(L"waddle_vulkan_experimental.dll"))result=1;
    if (!result)
        puts("DXVK real device, exact pixels/compute, swapchain Present and teardown succeeded on the selected ICD.");
    return result;
}
