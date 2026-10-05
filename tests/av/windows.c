/** @file windows.c @brief Native window lifecycle and C/WinRT boundary fixture. */
#include <windows.h>
#include <mmsystem.h>
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
static int defer_creation = 1;
static unsigned captured_frames;
static uint64_t minimum_timestamp;
static unsigned paint_sequence;
static LRESULT CALLBACK fixture_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT && (uint64_t)(uintptr_t)window == target_id) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(63, 127, 191));
        FillRect(dc, &paint.rcPaint, brush);
        SetPixelV(dc, 20, 20, RGB(++paint_sequence % 256, 0, 0));
        DeleteObject(brush); EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static HRESULT captured_pixels(const uint8_t *pixels, size_t length, uint32_t stride,
                               uint32_t width, uint32_t height, uint64_t timestamp_ns,
                               void *context) {
    (void)context;
    if (timestamp_ns < minimum_timestamp) return S_OK;
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
static void native_audio_test(void) {
    av_wasapi_t audio = {0};
    HRESULT status = av_wasapi_init(&audio, GetCurrentProcessId());
    fprintf(stderr, "Native process loopback: 0x%08lx\n", (unsigned long)status);
    assert(SUCCEEDED(status));
    WAVEFORMATEX format = {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 2,
        .nSamplesPerSec = 48000, .nAvgBytesPerSec = 192000, .nBlockAlign = 4, .wBitsPerSample = 16};
    HWAVEOUT render = NULL;
    assert(waveOutOpen(&render, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR);
    int16_t samples[96000];
    for (size_t i = 0; i < 96000; ++i) samples[i] = ((i / 2) % 96 < 48) ? 2048 : -2048;
    WAVEHDR packet = {.lpData = (LPSTR)samples, .dwBufferLength = sizeof(samples)};
    assert(waveOutPrepareHeader(render, &packet, sizeof(packet)) == MMSYSERR_NOERROR);
    audio_ring_header_t ring = {.sample_rate = AvSampleRate, .channels = 2,
        .format = 1, .capacity_frames = AvAudioCapacity};
    uint8_t pcm[AvAudioCapacity * AvAudioFrameBytes], consumed[1024];
    assert(waveOutWrite(render, &packet, sizeof(packet)) == MMSYSERR_NOERROR);
    unsigned nonzero = 0;
    ULONGLONG deadline = GetTickCount64() + 1500;
    while (GetTickCount64() < deadline) {
        WaitForSingleObject(audio.ready_event, 10);
        assert(SUCCEEDED(av_wasapi_drain(&audio, &ring, pcm, sizeof(pcm))));
        int frames = av_audio_read(&ring, pcm, sizeof(pcm), consumed, sizeof(consumed), 0);
        assert(frames >= 0);
        for (int i = 0; i < frames * 4; ++i) nonzero += consumed[i] != 0;
    }
    assert(waveOutReset(render) == MMSYSERR_NOERROR);
    assert(waveOutUnprepareHeader(render, &packet, sizeof(packet)) == MMSYSERR_NOERROR);
    assert(waveOutClose(render) == MMSYSERR_NOERROR);
    fprintf(stderr, "Native process loopback: frames=%llu nonzero_bytes=%u\n",
            (unsigned long long)audio.captured_frames, nonzero);
    assert(audio.captured_frames && nonzero);
    av_wasapi_free(&audio);
    av_wasapi_free(&audio);
}
/** @brief Fixed diagnostic accumulator owned by the calling capture thread. */
typedef struct capture_measurement_t {
    uint64_t last_timestamp;
    uint64_t age_sum_ns;
    uint64_t age_max_ns;
    uint64_t interval_max_ns;
    unsigned frames;
    unsigned duplicate_frames;
    unsigned invalid_timestamps;
    unsigned age_samples;
} capture_measurement_t;
static uint64_t performance_time_ns(void) {
    LARGE_INTEGER clock, frequency;
    assert(QueryPerformanceCounter(&clock) && QueryPerformanceFrequency(&frequency));
    return (uint64_t)(clock.QuadPart / frequency.QuadPart) * 1000000000 +
        (uint64_t)(clock.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart;
}
static HRESULT measured_pixels(const uint8_t *pixels, size_t length, uint32_t stride,
                               uint32_t width, uint32_t height, uint64_t timestamp_ns,
                               void *context) {
    capture_measurement_t *measurement = context;
    HRESULT result = captured_pixels(pixels, length, stride, width, height, timestamp_ns, NULL);
    if (timestamp_ns < minimum_timestamp) return result;
    if (timestamp_ns == measurement->last_timestamp) {
        ++measurement->duplicate_frames;
        return result;
    }
    uint64_t now = performance_time_ns();
    if (timestamp_ns <= measurement->last_timestamp || timestamp_ns > now) {
        ++measurement->invalid_timestamps;
        ++measurement->frames;
        measurement->last_timestamp = timestamp_ns;
        return result;
    }
    ++measurement->age_samples;
    uint64_t age = now - timestamp_ns;
    uint64_t interval = measurement->last_timestamp ? timestamp_ns - measurement->last_timestamp : 0;
    measurement->age_sum_ns += age;
    if (age > measurement->age_max_ns) measurement->age_max_ns = age;
    if (interval > measurement->interval_max_ns) measurement->interval_max_ns = interval;
    measurement->last_timestamp = timestamp_ns;
    ++measurement->frames;
    return result;
}
static int native_capture_benchmark(av_wgc_t *capture, av_wgc_read_t read_frame) {
    ShowWindow((HWND)(uintptr_t)target_id, SW_SHOW);
    DEVMODEW mode = {.dmSize = sizeof(mode)};
    assert(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode));
    fprintf(stderr, "Native display: %lux%lu @ %lu Hz\n", (unsigned long)mode.dmPelsWidth,
            (unsigned long)mode.dmPelsHeight, (unsigned long)mode.dmDisplayFrequency);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -10000};
    assert(timer && SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE));
    capture_measurement_t measurement = {0};
    minimum_timestamp = performance_time_ns();
    uint64_t start = performance_time_ns(), deadline = start + 3000000000;
    do {
        InvalidateRect((HWND)(uintptr_t)target_id, NULL, FALSE);
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        assert(SUCCEEDED(read_frame(capture, measured_pixels, &measurement)));
        assert(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0);
    } while (performance_time_ns() < deadline);
    uint64_t elapsed = performance_time_ns() - start;
    assert(CancelWaitableTimer(timer) && CloseHandle(timer));
    assert(measurement.frames > 1);
    fprintf(stderr, "Native capture benchmark: %u unique frames, %u duplicates in %.3f ms (%.2f fps)\n",
            measurement.frames, measurement.duplicate_frames, elapsed / 1000000.0,
            measurement.frames * 1000000000.0 / elapsed);
    fprintf(stderr, "WGC timestamp-to-CPU callback age: mean=%.3f ms max=%.3f ms; max frame interval=%.3f ms\n",
            measurement.age_samples ? measurement.age_sum_ns / (double)measurement.age_samples / 1000000.0 : 0.0,
            measurement.age_max_ns / 1000000.0, measurement.interval_max_ns / 1000000.0);
    fprintf(stderr, "WGC timing validation: %u invalid timestamps; age result %s\n",
            measurement.invalid_timestamps, measurement.invalid_timestamps ? "INVALID" : "valid");
    fputs("Capture diagnostics only: host presentation and audio output latency are unmeasured\n", stderr);
    return measurement.invalid_timestamps ? 1 : 0;
}
static int notification(const av_message_t *message, void *context) {
    (void)context;
    assert(message->window_id == target_id);
    if (message->type == MsgWindowCreate) {
        if (defer_creation) { defer_creation = 0; return 1; }
        ++creates;
    }
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
    int benchmark_status = 0;
    int benchmark = argc == 2 && !strcmp(argv[1], "--benchmark");
    int native_capture = benchmark || (argc == 2 && !strcmp(argv[1], "--capture"));
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
    assert(creates == 0);
    assert(av_windows_refresh() == 0);
    assert(creates == 1);
    assert(av_windows_start(GetCurrentProcessId(), notification, NULL) == -1);
    av_message_t resize = {.type = MsgWindowGeometry, .window_id = target_id,
                           .width = 500, .height = 350, .dpi = 96};
    assert(av_windows_apply(&resize) == 0);
    pump();
    assert(geometries >= 1);
    LONG_PTR original_style = GetWindowLongPtrW(target, GWL_STYLE);
    resize.flags = AvWindowFullscreen;
    assert(av_windows_apply(&resize) == 0);
    pump();
    assert(!(GetWindowLongPtrW(target, GWL_STYLE) & WS_OVERLAPPEDWINDOW));
    resize.flags = 0;
    assert(av_windows_apply(&resize) == 0);
    pump();
    assert(GetWindowLongPtrW(target, GWL_STYLE) == original_style);
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
        LARGE_INTEGER clock, frequency;
        assert(QueryPerformanceCounter(&clock) && QueryPerformanceFrequency(&frequency));
        minimum_timestamp = (uint64_t)(clock.QuadPart / frequency.QuadPart) * 1000000000 +
            (uint64_t)(clock.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart;
        capture_until_frame(capture, read_frame);
        if (benchmark) {
            ShowWindow(tool, SW_HIDE);
            benchmark_status = native_capture_benchmark(capture, read_frame);
        }
        destroy(&capture);
        assert(!capture);
        puts("Native WGC: exact center pixels survive an occluding window");
        native_audio_test();
    }
    FreeLibrary(library);
    resize.flags = AvWindowFullscreen;
    assert(av_windows_apply(&resize) == 0);
    av_windows_stop();
    assert(GetWindowLongPtrW(target, GWL_STYLE) == original_style);
    assert(av_windows_start(GetCurrentProcessId(), notification, NULL) == 0);
    assert(DestroyWindow(target)); pump();
    assert(destroys == 1);
    av_windows_stop(); av_windows_stop();
    assert(DestroyWindow(tool));
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    CoUninitialize();
    puts("Native AV: window filtering, DWM geometry, lifecycle and WinRT C ABI passed");
    return benchmark_status;
}
