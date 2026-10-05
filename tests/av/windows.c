/** @file windows.c @brief Native window lifecycle and C/WinRT boundary fixture. */
#include <windows.h>
#include "av_windows.h"
#include "av_guest_setup.h"
#include "av_wasapi.h"
#include "av_wgc.h"
#include "av_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t target_id;
static unsigned creates, geometries, destroys;
static unsigned captured_frames;
static LRESULT CALLBACK fixture_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT && (uint64_t)(uintptr_t)window == target_id) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(63, 127, 191));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush); EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static HRESULT captured_pixels(const uint8_t *pixels, size_t length, uint32_t stride,
                               uint32_t width, uint32_t height, uint64_t timestamp_ns,
                               void *context) {
    (void)context;
    assert(width > 100 && height > 100 && timestamp_ns);
    assert(stride >= width * 4 && length >= (size_t)stride * height);
    const uint8_t *center = pixels + (size_t)(height / 2) * stride + (width / 2) * 4;
    assert(center[0] == 191 && center[1] == 127 && center[2] == 63);
    ++captured_frames;
    return S_OK;
}
static void capture_until_frame(av_wgc_t *capture, av_wgc_read_t read_frame) {
    unsigned before = captured_frames;
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (captured_frames == before && GetTickCount64() < deadline) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        HRESULT result = read_frame(capture, captured_pixels, NULL);
        assert(SUCCEEDED(result));
        InvalidateRect((HWND)(uintptr_t)target_id, NULL, FALSE);
        Sleep(1);
    }
    fprintf(stderr, "Native WGC frames: %u -> %u\n", before, captured_frames);
    assert(captured_frames > before);
}
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
int main(int argc, char **argv) {
    int native_capture = argc == 2 && !strcmp(argv[1], "--capture");
    assert(av_guest_setup("relative") == 2);
    assert(av_guest_setup("C:\\bad\"path") == 2);

    assert(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
    WNDCLASSW window_class = {.lpfnWndProc = fixture_window_proc, .hInstance = GetModuleHandleW(NULL),
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
    if (native_capture) {
        HDC dc = GetDC(target);
        RECT client;
        assert(dc && GetClientRect(target, &client));
        HBRUSH brush = CreateSolidBrush(RGB(63, 127, 191));
        assert(brush && FillRect(dc, &client, brush));
        DeleteObject(brush); ReleaseDC(target, dc);
        HRESULT status = create(target, &capture);
        fprintf(stderr, "Native WGC creation: 0x%08lx\n", (unsigned long)status);
        assert(SUCCEEDED(status) && capture);
        av_wgc_read_t read_frame = (av_wgc_read_t)GetProcAddress(library, "av_wgc_read");
        assert(read_frame);
        capture_until_frame(capture, read_frame);
        assert(SetWindowPos(tool, HWND_TOPMOST, 100, 100, 700, 500, SWP_SHOWWINDOW));
        capture_until_frame(capture, read_frame);
        destroy(&capture);
        assert(!capture);
        puts("Native WGC: exact center pixels survive an occluding window");
    }
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
