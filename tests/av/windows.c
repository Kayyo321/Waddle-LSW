/** @file windows.c @brief Native window lifecycle and C/WinRT boundary fixture. */
#include <windows.h>
#include "av_windows.h"
#include "av_wasapi.h"
#include "av_wgc.h"
#include "av_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t target_id;
static unsigned creates, geometries, destroys;
static int notification(const av_message_t *message, void *context) {
    (void)context;
    assert(message->window_id == target_id);
    if (message->type == MsgWindowCreate) ++creates;
    else if (message->type == MsgWindowGeometry) ++geometries;
    else if (message->type == MsgWindowDestroy) ++destroys;
    else assert(0);
    return 0;
}
static void pump(void) {
    ULONGLONG deadline = GetTickCount64() + 300;
    do {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
}
int main(void) {
    assert(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
    WNDCLASSW window_class = {.lpfnWndProc = DefWindowProcW, .hInstance = GetModuleHandleW(NULL),
                              .lpszClassName = L"WaddleAvFixture"};
    assert(RegisterClassW(&window_class));
    HWND target = CreateWindowExW(0, window_class.lpszClassName, L"Waddle AV 日本語",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, NULL, NULL, window_class.hInstance, NULL);
    HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW, window_class.lpszClassName, L"Filtered tool",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 800, 100, 100, 100, NULL, NULL, window_class.hInstance, NULL);
    assert(target && tool);
    target_id = (uint64_t)(uintptr_t)target;
    assert(av_windows_start(0, notification, NULL) == -1);
    assert(av_windows_start(GetCurrentProcessId(), notification, NULL) == 0);
    assert(creates == 1);
    assert(av_windows_start(GetCurrentProcessId(), notification, NULL) == -1);
    av_message_t resize = {.type = MsgWindowGeometry, .window_id = target_id,
                           .width = 500, .height = 350, .dpi = 96};
    assert(av_windows_apply(&resize) == 0);
    pump();
    assert(geometries >= 1);
    av_message_t stale = {.type = MsgWindowClose, .window_id = 1};
    assert(av_windows_apply(&stale) == -1);
    av_wasapi_t audio = {0};
    assert(av_wasapi_init(&audio, 0) == E_INVALIDARG);
    av_wasapi_free(&audio);
    HMODULE library = LoadLibraryExW(L"av_wgc.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(library);
    av_wgc_create_t create = (av_wgc_create_t)GetProcAddress(library, "av_wgc_create");
    av_wgc_destroy_t destroy = (av_wgc_destroy_t)GetProcAddress(library, "av_wgc_destroy");
    assert(create && destroy);
    av_wgc_t *capture = NULL;
    assert(create(NULL, &capture) == E_INVALIDARG && !capture);
    destroy(&capture);
    FreeLibrary(library);
    assert(DestroyWindow(target)); pump();
    assert(destroys == 1);
    av_windows_stop(); av_windows_stop();
    assert(DestroyWindow(tool));
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    CoUninitialize();
    puts("Native AV: window filtering, DWM geometry, lifecycle and WinRT C ABI passed");
    return 0;
}
