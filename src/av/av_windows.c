#include "av_windows.h"
#include <dwmapi.h>
#include <string.h>

/** @brief Tracker-thread owned handles; no allocation, lifetime start..stop. */
typedef struct tracked_window_t {
    HWND handle;
    av_message_t geometry;
} tracked_window_t;
static tracked_window_t windows[AvMaxWindows];
static HWINEVENTHOOK object_hook;
static HWINEVENTHOOK minimize_hook;
static DWORD target_process;
static av_window_notify_t notification;
static void *notification_context;
static int delivery_failed;

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
static void emit(av_message_t *message) {
    if (notification(message, notification_context) != 0)
        delivery_failed = 1;
}
static void remove_window(int index) {
    av_message_t message = {.type = MsgWindowDestroy,
                            .window_id = (uint64_t)(uintptr_t)windows[index].handle};
    emit(&message);
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
        windows[index].handle = handle;
        message.type = MsgWindowCreate;
        windows[index].geometry = message;
        emit(&message);
    } else {
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
    if (!process_id || !notify || notification)
        return -1;
    target_process = process_id;
    notification = notify;
    notification_context = context;
    delivery_failed = 0;
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
void av_windows_stop(void) {
    if (object_hook)
        UnhookWinEvent(object_hook);
    if (minimize_hook)
        UnhookWinEvent(minimize_hook);
    object_hook = NULL;
    minimize_hook = NULL;
    memset(windows, 0, sizeof(windows));
    notification = NULL;
    notification_context = NULL;
    target_process = 0;
}
int av_windows_apply(const av_message_t *message) {
    HWND handle = (HWND)(uintptr_t)message->window_id;
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    if (find_window(handle) < 0 || process_id != target_process || !IsWindow(handle))
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
    RECT rect;
    if (!GetWindowRect(handle, &rect))
        return -1;
    return SetWindowPos(handle, NULL, 0, 0, (int)message->width, (int)message->height,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
               ? 0
               : -1;
}
