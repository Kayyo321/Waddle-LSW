/** @file av_guest_setup.c @brief Native AV driver deployment and capability probe. */
#define COBJMACROS
#include "av_guest_setup.h"
#include "av_ivshmem.h"
#include <d3d11.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
/** @brief System RtlGetVersion procedure; borrowed ntdll lifetime, no retained data. */
typedef LONG (WINAPI *av_version_query_t)(OSVERSIONINFOW *version);
int av_guest_probe(void) {
    HMODULE system = GetModuleHandleW(L"ntdll.dll");
    av_version_query_t query = NULL;
    FARPROC procedure = system ? GetProcAddress(system, "RtlGetVersion") : NULL;
    memcpy(&query, &procedure, sizeof(query));
    OSVERSIONINFOW version = {.dwOSVersionInfoSize = sizeof(version)};
    if (!query || query(&version) != 0 || version.dwBuildNumber < 20348) {
        fputs("AV probe: Windows build 20348 or later is required\n", stderr);
        return 1;
    }
    av_ivshmem_t memory = {0};
    if (av_ivshmem_init(&memory, 0) != 0) {
        fputs("AV probe: signed IVSHMEM device or fixed mapping ABI is unavailable\n", stderr);
        return 1;
    }
    av_ivshmem_free(&memory);
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return 1;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *endpoint = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    int result = 1;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                               &IID_IMMDeviceEnumerator, (void **)&enumerator)) ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &endpoint))) {
        fputs("AV probe: an enabled Windows render endpoint is required\n", stderr);
        goto cleanup;
    }
    HRESULT graphics = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
    if (FAILED(graphics)) {
        graphics = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context);
        if (SUCCEEDED(graphics))
            puts("AV probe: software WARP capture fallback; GPU performance is unavailable");
    }
    if (FAILED(graphics)) {
        fputs("AV probe: neither hardware D3D11 nor software WARP is available\n", stderr);
        goto cleanup;
    }
    WCHAR adapter_path[32768];
    DWORD length = GetModuleFileNameW(NULL, adapter_path, 32768);
    if (!length || length >= 32768)
        goto cleanup;
    WCHAR *slash = wcsrchr(adapter_path, L'\\');
    if (!slash || (size_t)(slash - adapter_path) + 12 >= 32768)
        goto cleanup;
    wcscpy(slash + 1, L"av_wgc.dll");
    HMODULE adapter = LoadLibraryExW(adapter_path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!adapter) {
        fputs("AV probe: bundled WinRT capture adapter is unavailable\n", stderr);
        goto cleanup;
    }
    int valid = GetProcAddress(adapter, "av_wgc_create") &&
                GetProcAddress(adapter, "av_wgc_read") && GetProcAddress(adapter, "av_wgc_destroy");
    FreeLibrary(adapter);
    if (!valid)
        goto cleanup;
    printf("AV probe: Windows build %lu, IVSHMEM mapping, D3D11, adapter and render endpoint ready\n",
           (unsigned long)version.dwBuildNumber);
    result = 0;
cleanup:
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (endpoint) IMMDevice_Release(endpoint);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize();
    return result;
}
int av_guest_setup(const char *directory) {
    size_t length = strlen(directory);
    if (!length || length > 1023 || strchr(directory, '"') || strchr(directory, '\r') || strchr(directory, '\n'))
        return 2;
    WCHAR path[1024], executable[32768], command[35000];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, path, 1024) ||
        !((path[0] && path[1] == L':' && path[2] == L'\\') || (path[0] == L'\\' && path[1] == L'\\')))
        return 2;
    UINT count = GetSystemDirectoryW(executable, 32768);
    if (!count || count + 13 >= 32768)
        return 1;
    wcscat(executable, L"\\pnputil.exe");
    int written = _snwprintf(command, 35000, L"\"%ls\" /add-driver \"%ls\\ivshmem.inf\" /install", executable, path);
    if (written <= 0 || written >= 35000)
        return 2;
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "AV setup: driver installation needs guest administrator access (%lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }
    CloseHandle(process.hThread);
    DWORD waited = WaitForSingleObject(process.hProcess, 120000), status = 1;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    } else if (waited == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (status == 3010 || status == 1641) {
        fputs("AV setup: driver installed; reboot required before readiness can be verified\n", stderr);
        return 3;
    }
    /* PnPUtil returns ERROR_NO_MORE_ITEMS when the signed package is already
     * current. Readiness still requires the actual driver mapping probe. */
    if (status != 0 && status != ERROR_NO_MORE_ITEMS) {
        fprintf(stderr, "AV setup: Windows rejected driver installation (%lu)\n", (unsigned long)status);
        return 1;
    }
    return av_guest_probe();
}
