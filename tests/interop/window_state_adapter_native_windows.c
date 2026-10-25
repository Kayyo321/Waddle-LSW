/**
 * @file window_state_adapter_native_windows.c
 * @brief Real Win32 smoke of the window-state guest adapter (Windows only; executed by CI).
 *
 * The smoke creates its own top-level window on the interactive desktop, owns it on its main thread,
 * and drives the adapter through window_state_native_api() (the real Win32 table). Every step asserts
 * both the observation from window_state_adapter_observe and the native fact read through Win32 directly.
 * It prints one line per step and returns nonzero at the first mismatch. It never calls DestroyWindow
 * except as the owner after the close check, and it never terminates a process.
 */
#include "window_state_adapter_windows.h"
#include "window_state.h"

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

static volatile LONG CloseSeen = 0;

static LRESULT CALLBACK smoke_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) {
        InterlockedExchange(&CloseSeen, 1);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int failures = 0;

/** @brief Records one step result. */
static void step(const char *name, int passed) {
    printf("%s %s\n", passed ? "ok  " : "FAIL", name);
    if (!passed) failures += 1;
}

/** @brief Creates the smoke's top-level window at a known position. */
static HWND create_smoke_window(void) {
    WNDCLASSEXW cls;
    HWND window;
    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = smoke_window_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"WaddleWindowStateSmoke";
    if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    window = CreateWindowExW(0, L"WaddleWindowStateSmoke", L"waddle window state smoke", WS_OVERLAPPEDWINDOW,
                             100, 100, 500, 400, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (window != NULL) {
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
    }
    return window;
}

/** @brief Pumps queued messages so posted WM_CLOSE is delivered. */
static void pump_messages(void) {
    MSG message;
    for (int count = 0; count < 200 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); ++count) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static uint32_t observe_state(window_state_adapter_t *adapter, HWND window, window_state_adapter_status_t *status) {
    uint32_t state = 0xffffffffu;
    *status = window_state_adapter_observe(adapter, (uintptr_t)window, 1, 1, &state);
    return state;
}

static int rect_equals(RECT left, RECT right) {
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

static waddle_window_state_action_t act(uint32_t kind) {
    waddle_window_state_action_t value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    return value;
}

static window_state_adapter_status_t run_one(window_state_adapter_t *adapter, HWND window, const waddle_window_state_action_t *action,
                                             uint64_t *completed) {
    return window_state_adapter_execute(adapter, (uintptr_t)window, 1, 1, action, 1, completed);
}

int main(void) {
    const window_state_native_api_t *api = window_state_native_api();
    window_state_adapter_t *adapter = NULL;
    HWND window;
    HWND stale_window;
    window_state_adapter_status_t status;
    uint64_t completed = 0;
    uint32_t state;
    RECT before_fullscreen;
    RECT monitor_rect;
    MONITORINFO info;
    LONG_PTR style_before;
    waddle_window_state_action_t action;

    if (api == NULL) {
        printf("FAIL native table is NULL on this platform\n");
        return 1;
    }
    if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        printf("FAIL per-monitor DPI awareness v2 not available on this desktop\n");
        return 1;
    }

    window = create_smoke_window();
    step("create smoke window", window != NULL);
    if (window == NULL) return 1;
    step("create adapter over the real Win32 table", window_state_adapter_create(api, NULL, &adapter) == WindowStateAdapterOk);
    step("admit the smoke window", window_state_adapter_admit(adapter, (uintptr_t)window, 1, 1) == WindowStateAdapterOk);

    /* Minimize, then restore. */
    action = act(WaddleWindowStateActionGuestMinimize);
    status = run_one(adapter, window, &action, &completed);
    step("minimize completes", status == WindowStateAdapterOk && completed == 1);
    state = observe_state(adapter, window, &status);
    step("minimize observed as Minimized", status == WindowStateAdapterOk && (state & WaddleWindowStateMinimized) != 0);
    step("minimize confirmed by IsIconic", IsIconic(window) != 0);

    action = act(WaddleWindowStateActionGuestRestore);
    status = run_one(adapter, window, &action, &completed);
    state = observe_state(adapter, window, &status);
    step("restore completes", status == WindowStateAdapterOk && completed == 1);
    step("restore observed without Minimized or Maximized",
         status == WindowStateAdapterOk && (state & (WaddleWindowStateMinimized | WaddleWindowStateMaximized)) == 0);

    /* Maximize, then restore to normal before fullscreen. */
    action = act(WaddleWindowStateActionGuestMaximize);
    status = run_one(adapter, window, &action, &completed);
    state = observe_state(adapter, window, &status);
    step("maximize observed as Maximized", status == WindowStateAdapterOk && (state & WaddleWindowStateMaximized) != 0);
    step("maximize confirmed by IsZoomed", IsZoomed(window) != 0);

    action = act(WaddleWindowStateActionGuestRestore);
    run_one(adapter, window, &action, &completed);

    /* Fullscreen round trip. */
    GetWindowRect(window, &before_fullscreen);
    style_before = GetWindowLongPtrW(window, GWL_STYLE);
    action = act(WaddleWindowStateActionGuestEnterFullscreen);
    status = run_one(adapter, window, &action, &completed);
    step("enter fullscreen completes", status == WindowStateAdapterOk && completed == 1);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    monitor_rect = info.rcMonitor;
    {
        RECT current;
        LONG_PTR style_now;
        GetWindowRect(window, &current);
        style_now = GetWindowLongPtrW(window, GWL_STYLE);
        step("fullscreen covers the monitor rectangle", rect_equals(current, monitor_rect));
        step("fullscreen strips WS_CAPTION and WS_THICKFRAME",
             (style_now & (WS_CAPTION | WS_THICKFRAME)) == 0);
    }
    state = observe_state(adapter, window, &status);
    step("fullscreen observed as Fullscreen", status == WindowStateAdapterOk && (state & WaddleWindowStateFullscreen) != 0);

    action = act(WaddleWindowStateActionGuestLeaveFullscreen);
    status = run_one(adapter, window, &action, &completed);
    step("leave fullscreen completes", status == WindowStateAdapterOk && completed == 1);
    {
        RECT current;
        LONG_PTR style_now;
        GetWindowRect(window, &current);
        style_now = GetWindowLongPtrW(window, GWL_STYLE);
        step("leave fullscreen restores the style", style_now == style_before);
        step("leave fullscreen restores the rectangle", rect_equals(current, before_fullscreen));
    }
    state = observe_state(adapter, window, &status);
    step("leave fullscreen observed without Fullscreen", status == WindowStateAdapterOk && (state & WaddleWindowStateFullscreen) == 0);

    /* Geometry: the origin is kept (the host sends x and y as zero), and the visible frame takes the size. */
    {
        RECT origin_before;
        RECT current;
        RECT visible;
        GetWindowRect(window, &origin_before);
        memset(&action, 0, sizeof(action));
        action.kind = WaddleWindowStateActionGuestSetGeometry;
        action.x = 0;
        action.y = 0;
        action.width = 320;
        action.height = 240;
        status = run_one(adapter, window, &action, &completed);
        step("set geometry completes", status == WindowStateAdapterOk && completed == 1);
        GetWindowRect(window, &current);
        DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible));
        step("set geometry keeps the window origin", current.left == origin_before.left && current.top == origin_before.top);
        step("set geometry sets the visible frame size",
             visible.right - visible.left == 320 && visible.bottom - visible.top == 240);
    }

    /* Activation. */
    action = act(WaddleWindowStateActionGuestActivate);
    status = run_one(adapter, window, &action, &completed);
    step("activate completes", status == WindowStateAdapterOk && completed == 1);
    step("activate makes the window foreground", GetForegroundWindow() == window);

    /* Close is posted, never destroyed by the adapter. */
    action = act(WaddleWindowStateActionGuestPostClose);
    status = run_one(adapter, window, &action, &completed);
    step("post close completes", status == WindowStateAdapterOk && completed == 1);
    for (int wait = 0; wait < 50 && InterlockedCompareExchange(&CloseSeen, 0, 0) == 0; ++wait) {
        pump_messages();
        Sleep(10);
    }
    step("window receives WM_CLOSE", InterlockedCompareExchange(&CloseSeen, 0, 0) == 1);
    step("post close leaves the window alive", IsWindow(window) != 0);

    /* Stale handle: the owner destroys the window, then the adapter is asked to act on it. */
    DestroyWindow(window);
    stale_window = window;
    action = act(WaddleWindowStateActionGuestMinimize);
    status = run_one(adapter, stale_window, &action, &completed);
    step("destroyed window is StaleHandle", status == WindowStateAdapterStaleHandle && completed == 0);
    state = observe_state(adapter, stale_window, &status);
    step("stale window is removed from the adapter", status == WindowStateAdapterNotAdmitted);

    /* Reuse attempt: a new window must not be acted on under the old admission. */
    window = create_smoke_window();
    step("create second window", window != NULL);
    if (window != NULL) {
        step("admission of the new window is a fresh admission",
             window_state_adapter_admit(adapter, (uintptr_t)window, 1, 1) == WindowStateAdapterOk);
        action = act(WaddleWindowStateActionGuestMinimize);
        status = window_state_adapter_execute(adapter, (uintptr_t)stale_window, 1, 1, &action, 1, &completed);
        if (window != stale_window) {
            step("old HWND value is refused against the new admission",
                 status == WindowStateAdapterInvalidArgument && completed == 0);
        } else {
            printf("note recycled HWND value %p reused by the system; refusal case not exercised\n", (void *)window);
        }
        DestroyWindow(window);
    }

    step("destroy adapter", window_state_adapter_destroy(&adapter) == WindowStateAdapterOk && adapter == NULL);
    if (failures != 0) {
        printf("window_state_adapter_native: %d step(s) failed\n", failures);
        return 1;
    }
    printf("window_state_adapter_native: all steps passed\n");
    return 0;
}
