#include "av_windows.h"
#include "av_input.h"
#include <dwmapi.h>
#include <string.h>
#include <limits.h>

/** @brief Tracker-thread owned handles; no allocation, lifetime start..stop. */
typedef struct tracked_window_t {
    HWND handle;
    av_message_t geometry;
    LONG_PTR saved_style;
    RECT saved_bounds;
    int fullscreen_override;
} tracked_window_t;
static tracked_window_t windows[AvMaxWindows];
static HWINEVENTHOOK object_hook;
static HWINEVENTHOOK minimize_hook;
static DWORD target_process;
static av_window_notify_t notification;
static void *notification_context;
static int delivery_failed;
static uint64_t next_incarnation;
static av_input_state_t input_state;
static DPI_AWARENESS_CONTEXT previous_dpi_context;
static const av_input_ops_t InputOps;

static void restore_window(tracked_window_t *window) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window->handle, &process_id);
    if (window->fullscreen_override && process_id == target_process && IsWindow(window->handle)) {
        SetWindowLongPtrW(window->handle, GWL_STYLE, window->saved_style);
        SetWindowPos(window->handle, NULL, window->saved_bounds.left, window->saved_bounds.top,
            window->saved_bounds.right - window->saved_bounds.left,
            window->saved_bounds.bottom - window->saved_bounds.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    window->fullscreen_override = 0;
}

static int find_window(HWND handle) {
    for (unsigned i = 0; i < AvMaxWindows; ++i)
        if (windows[i].handle == handle)
            return (int)i;
    return -1;
}
static int eligible(HWND handle) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    return process_id == target_process && IsWindowVisible(handle) &&
           GetAncestor(handle, GA_ROOT) == handle &&
           !(style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
           (!GetWindow(handle, GW_OWNER) || (style & WS_EX_APPWINDOW));
}
static int geometry(HWND handle, av_message_t *message) {
    RECT bounds;
    WCHAR title[256];
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    if (process_id != target_process || !IsWindow(handle))
        return -1;
    if (FAILED(
            DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        !GetWindowRect(handle, &bounds))
        return -1;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        (int64_t)bounds.right - bounds.left > 8192 || (int64_t)bounds.bottom - bounds.top > 8192)
        return -1;
    memset(message, 0, sizeof(*message));
    message->window_id = (uint64_t)(uintptr_t)handle;
    message->process_id = process_id;
    message->x = bounds.left;
    message->y = bounds.top;
    message->width = (uint32_t)(bounds.right - bounds.left);
    message->height = (uint32_t)(bounds.bottom - bounds.top);
    message->dpi = GetDpiForWindow(handle);
    if (!message->dpi)
        message->dpi = 96;
    if (IsIconic(handle))
        message->flags |= AvWindowMinimized;
    if (GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) &&
        bounds.left <= monitor.rcMonitor.left && bounds.top <= monitor.rcMonitor.top &&
        bounds.right >= monitor.rcMonitor.right && bounds.bottom >= monitor.rcMonitor.bottom)
        message->flags |= AvWindowFullscreen;
    int count = GetWindowTextW(handle, title, 256);
    if (count > 0 && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, title, count,
                                          message->title, 255, NULL, NULL))
        message->title[0] = '\0';
    return 0;
}
static int emit(av_message_t *message) {
    int result = notification(message, notification_context);
    if (result < 0 || (result > 0 && message->type != MsgWindowCreate))
        delivery_failed = 1;
    return result;
}
static void remove_window(int index) {
    if (input_state.window_id == windows[index].geometry.window_id &&
        input_state.incarnation == windows[index].geometry.sequence &&
        av_input_reset(&input_state, &InputOps, NULL) != 0)
        delivery_failed = 1;
    av_message_t message = {.type = MsgWindowDestroy,
                            .window_id = (uint64_t)(uintptr_t)windows[index].handle};
    emit(&message);
    restore_window(&windows[index]);
    memset(&windows[index], 0, sizeof(windows[index]));
}
static void update_window(HWND handle) {
    int index = find_window(handle);
    if (!eligible(handle)) {
        if (index >= 0 && !IsIconic(handle))
            remove_window(index);
        return;
    }
    av_message_t message;
    if (geometry(handle, &message) != 0)
        return;
    if (index < 0) {
        for (unsigned i = 0; i < AvMaxWindows; ++i) {
            if (!windows[i].handle) {
                index = (int)i;
                break;
            }
        }
        if (index < 0)
            return; /* Bounded registry: next event retries admission. */
        if (next_incarnation == UINT64_MAX) { delivery_failed = 1; return; }
        message.sequence = ++next_incarnation;
        windows[index].handle = handle;
        message.type = MsgWindowCreate;
        windows[index].geometry = message;
        if (emit(&message) > 0) memset(&windows[index], 0, sizeof(windows[index]));
    } else {
        message.sequence = windows[index].geometry.sequence;
        message.type = MsgWindowGeometry;
        av_message_t previous = windows[index].geometry;
        previous.type = MsgWindowGeometry;
        if (memcmp(&previous, &message, sizeof(message)) != 0) {
            windows[index].geometry = message;
            emit(&message);
        }
    }
}
static void CALLBACK window_event(HWINEVENTHOOK hook, DWORD event, HWND handle, LONG object_id,
                                  LONG child_id, DWORD thread_id, DWORD event_time) {
    (void)hook;
    (void)thread_id;
    (void)event_time;
    if (!handle || child_id != CHILDID_SELF)
        return;
    if (event >= EVENT_OBJECT_CREATE && object_id != OBJID_WINDOW)
        return;
    int index = find_window(handle);
    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) {
        if (index >= 0)
            remove_window(index);
    } else if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW ||
               event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_MINIMIZESTART ||
               event == EVENT_SYSTEM_MINIMIZEEND)
        update_window(handle);
}
static BOOL CALLBACK enumerate_window(HWND handle, LPARAM context) {
    (void)context;
    update_window(handle);
    return TRUE;
}
int av_windows_start(DWORD process_id, av_window_notify_t notify, void *context) {
    if (!process_id || !notify || notification || input_state.window_id)
        return -1;
    previous_dpi_context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!previous_dpi_context) return -1;
    target_process = process_id;
    notification = notify;
    notification_context = context;
    delivery_failed = 0;
    memset(&input_state, 0, sizeof(input_state));
    object_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
                                  window_event, process_id, 0, WINEVENT_OUTOFCONTEXT);
    minimize_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL,
                                    window_event, process_id, 0, WINEVENT_OUTOFCONTEXT);
    if (!object_hook || !minimize_hook) {
        av_windows_stop();
        return -1;
    }
    EnumWindows(enumerate_window, 0);
    if (delivery_failed) {
        av_windows_stop();
        return -1;
    }
    return 0;
}
int av_windows_stop(void) {
    int released = av_input_reset(&input_state, &InputOps, NULL);
    if (released != 0) released = av_input_reset(&input_state, &InputOps, NULL);
    if (object_hook)
        UnhookWinEvent(object_hook);
    if (minimize_hook)
        UnhookWinEvent(minimize_hook);
    object_hook = NULL;
    minimize_hook = NULL;
    for (unsigned i = 0; i < AvMaxWindows; ++i) restore_window(&windows[i]);
    memset(windows, 0, sizeof(windows));
    notification = NULL;
    notification_context = NULL;
    target_process = 0;
    if (previous_dpi_context) {
        if (!SetThreadDpiAwarenessContext(previous_dpi_context)) released = -1;
        previous_dpi_context = NULL;
    }
    return released;
}
int av_windows_refresh(void) {
    if (!target_process || !notification) return -1;
    if (input_state.window_id && GetForegroundWindow() != (HWND)(uintptr_t)input_state.window_id) {
        av_input_reset(&input_state, &InputOps, NULL);
        return -1; /* Foreground loss is terminal, never a silent dead input path. */
    }
    if (!EnumWindows(enumerate_window, 0)) return -1;
    return delivery_failed ? -1 : 0;
}
int av_windows_apply(const av_message_t *message) {
    HWND handle = (HWND)(uintptr_t)message->window_id;
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    int index = find_window(handle);
    if (index < 0 || process_id != target_process || !IsWindow(handle))
        return -1;
    if (message->type == MsgWindowClose)
        return PostMessageW(handle, WM_CLOSE, 0, 0) ? 0 : -1;
    if (message->type != MsgWindowGeometry || !message->width || !message->height ||
        message->width > 8192 || message->height > 8192 || message->flags > 3)
        return -1;
    if (message->flags & AvWindowMinimized) {
        ShowWindow(handle, SW_MINIMIZE);
        return 0;
    }
    if (IsIconic(handle))
        ShowWindow(handle, SW_RESTORE);
    tracked_window_t *window = &windows[index];
    if (message->flags & AvWindowFullscreen) {
        MONITORINFO monitor = {.cbSize = sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
            return -1;
        if (!window->fullscreen_override && !(window->geometry.flags & AvWindowFullscreen)) {
            if (!GetWindowRect(handle, &window->saved_bounds)) return -1;
            window->saved_style = GetWindowLongPtrW(handle, GWL_STYLE);
            SetLastError(0);
            if (!SetWindowLongPtrW(handle, GWL_STYLE, window->saved_style & ~WS_OVERLAPPEDWINDOW) && GetLastError())
                return -1;
            window->fullscreen_override = 1;
        }
        return SetWindowPos(handle, NULL, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE) ? 0 : -1;
    }
    if (window->fullscreen_override) {
        SetLastError(0);
        if (!SetWindowLongPtrW(handle, GWL_STYLE, window->saved_style) && GetLastError()) return -1;
        if (!SetWindowPos(handle, NULL, window->saved_bounds.left, window->saved_bounds.top,
            window->saved_bounds.right - window->saved_bounds.left,
            window->saved_bounds.bottom - window->saved_bounds.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE)) return -1;
        window->fullscreen_override = 0;
    }
    RECT rect;
    if (!GetWindowRect(handle, &rect))
        return -1;
    RECT visible = rect;
    DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible));
    int width = (int)message->width + (rect.right - rect.left) - (visible.right - visible.left);
    int height = (int)message->height + (rect.bottom - rect.top) - (visible.bottom - visible.top);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return -1;
    return SetWindowPos(handle, NULL, 0, 0, width, height,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
               ? 0
               : -1;
}

static int input_target(void *context, uint64_t window_id, uint64_t incarnation, int activate) {
    (void)context;
    HWND handle = (HWND)(uintptr_t)window_id;
    int index = find_window(handle);
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    if (index < 0 || windows[index].geometry.sequence != incarnation ||
        process_id != target_process || !IsWindow(handle)) return activate < 0 ? 1 : -1;
    if (activate < 0) return 0;
    if (!IsWindowVisible(handle) ||
        !IsWindowEnabled(handle) || IsIconic(handle)) return -1;
    if (activate && GetForegroundWindow() != handle && !SetForegroundWindow(handle)) return -1;
    return GetForegroundWindow() == handle ? 0 : -1;
}
static int point_owned(HWND handle, POINT point) {
    HWND hit = WindowFromPoint(point);
    return hit && GetAncestor(hit, GA_ROOT) == handle;
}
static int input_emit(void *context, const av_message_t *event) {
    (void)context;
    INPUT input = {0};
    if (event->type == MsgInputKey) {
        uint16_t scan = av_input_scan_code(event->width);
        if (!scan) return -1;
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = scan & 0xff;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0) |
                          (event->flags ? 0 : KEYEVENTF_KEYUP);
    } else {
        input.type = INPUT_MOUSE;
        HWND handle = (HWND)(uintptr_t)event->window_id;
        if (event->type == MsgInputPointer) {
            av_message_t bounds;
            if (geometry(handle, &bounds) != 0 || event->x < 0 || event->y < 0 ||
                (uint32_t)event->x >= bounds.width || (uint32_t)event->y >= bounds.height)
                return -1;
            int64_t screen_x = (int64_t)bounds.x + event->x;
            int64_t screen_y = (int64_t)bounds.y + event->y;
            int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int64_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int64_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int64_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1 || screen_x < left || screen_y < top ||
                screen_x >= left + width || screen_y >= top + height ||
                screen_x < LONG_MIN || screen_x > LONG_MAX ||
                screen_y < LONG_MIN || screen_y > LONG_MAX) return -1;
            POINT point = {(LONG)screen_x, (LONG)screen_y};
            if (!point_owned(handle, point)) return -1;
            input.mi.dx = (LONG)((screen_x - left) * 65535 / (width - 1));
            input.mi.dy = (LONG)((screen_y - top) * 65535 / (height - 1));
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        } else if (event->type == MsgInputButton) {
            static const DWORD Down[4] = {0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
            static const DWORD Up[4] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
            if (event->flags) {
                POINT point;
                if (!GetCursorPos(&point) || !point_owned(handle, point)) return -1;
            }
            input.mi.dwFlags = event->flags ? Down[event->width] : Up[event->width];
        } else if (event->type == MsgInputWheel) {
            POINT point;
            if (!GetCursorPos(&point) || !point_owned(handle, point)) return -1;
            // Horizontal and vertical wheel data share mouseData; inject separately.
            if (event->x) {
                input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
                input.mi.mouseData = (DWORD)event->x;
                if (SendInput(1, &input, sizeof(input)) != 1) return -1;
            }
            if (!event->y) return 0;
            if (input_target(NULL, event->window_id, event->sequence, 0) != 0) return -1;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = (DWORD)event->y;
        } else return -1;
    }
    return SendInput(1, &input, sizeof(input)) == 1 ? 0 : -1;
}
static const av_input_ops_t InputOps = {.target = input_target, .emit = input_emit};
int av_windows_input(const av_message_t *message) {
    if (!notification || !message) return -1;
    return av_input_apply(&input_state, message, &InputOps, NULL);
}
