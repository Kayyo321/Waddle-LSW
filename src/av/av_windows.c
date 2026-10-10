#include "av_windows.h"
#include "av_input.h"
#include "av_lease.h"
#include "av_identity.h"
/* The API-double fixture supplies a narrow SDK-shaped declaration set. */
#ifndef WaddleWindowsApiDouble
#include <dwmapi.h>
#endif
#include <string.h>
#include <limits.h>

/** @brief Tracker-thread owned handles; no allocation, lifetime start..stop. */
typedef struct tracked_window_t {
    HWND handle;
    av_message_t geometry;
    LONG_PTR saved_style;
    RECT saved_bounds;
    int fullscreen_override;
    int retiring;
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
static const av_lease_ops_t LeaseOps;
/** @brief Owner-thread value queue; callbacks never mutate registry/input state. */
#define AvNativeObservations 64u
typedef struct native_observation_t {
    HWINEVENTHOOK hook;
    DWORD event, thread_id, event_time;
    HWND handle;
    LONG object_id, child_id;
    uint64_t incarnation, order, activation_operation;
} native_observation_t;
static native_observation_t observations[AvNativeObservations];
static unsigned observation_head, observation_count;
static uint64_t observation_order, operation_order, last_clock;
static int lease_mode, operation_guard, callback_guard, reconcile_guard, stopping;
static int teardown_failed;
static int operation_release_failed; /* Failed cleanup is retried only by stop. */
static DWORD owner_thread;
static HANDLE target_lifetime;
static HWINEVENTHOOK foreground_hook;
static av_lease_guest_t lease_state;
static struct {
    int active, seen_target;
    uint64_t operation, window_id, incarnation;
} activation;
static int reconcile_native(void *context);
static int fail_native(void) { delivery_failed = 1; return -1; }
static int owner_begin(void) {
    if (!notification || delivery_failed || stopping || GetCurrentThreadId() != owner_thread ||
        operation_guard || operation_order == UINT64_MAX) return fail_native();
    operation_guard = 1;
    operation_release_failed = 0;
    ++operation_order;
    return 0;
}
static uint64_t native_now(void *context) {
    (void)context;
    uint64_t now = GetTickCount64();
    if (!now || now < last_clock) { fail_native(); return 0; }
    last_clock = now;
    return now;
}
static int input_desktop(void) {
    BOOL receiving = FALSE;
    DWORD needed = 0;
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId()); /* Borrowed: never close. */
    return desktop && GetUserObjectInformationW(desktop, UOI_IO, &receiving,
        sizeof(receiving), &needed) && needed == sizeof(receiving) && receiving ? 0 : -1;
}
static int lifetime_live(void) {
    return target_lifetime && GetProcessId(target_lifetime) == target_process &&
        WaitForSingleObject(target_lifetime, 0) == WAIT_TIMEOUT;
}
static int owner_end(int result, int reconcile) {
    if (lease_mode && result == 0 && reconcile && reconcile_native(NULL) != 0) result = -1;
    if (delivery_failed || (lease_mode && result < 0)) {
        delivery_failed = 1;
        if (lease_mode) {
            lease_state.phase = AvLeaseGuestTerminal;
            lease_state.deadline = 0;
            /* Outer boundary only: successful insertions have already committed holds. */
            if (!operation_release_failed)
                av_input_reset(&lease_state.input, &LeaseOps.input, NULL);
        }
        result = -1;
    }
    activation.active = 0;
    operation_guard = 0;
    return result;
}

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
    if (!handle) return -1;
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
/* This predicate deliberately differs from capture admission: owned APPWINDOWs
 * remain capturable, but never become recoverable input candidates. */
static int input_eligible(HWND handle, uint64_t incarnation) {
    int index = find_window(handle);
    DWORD process_id = 0;
    if (index < 0 || windows[index].retiring ||
        windows[index].geometry.sequence != incarnation || !lifetime_live()) return 0;
    GetWindowThreadProcessId(handle, &process_id);
    LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    return process_id == target_process && IsWindow(handle) && IsWindowVisible(handle) &&
        IsWindowEnabled(handle) && !IsIconic(handle) && GetAncestor(handle, GA_ROOT) == handle &&
        !GetWindow(handle, GW_OWNER) && !(style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
}
static int foreground_pair(HWND handle, uint64_t *incarnation) {
    int index = find_window(handle);
    if (index < 0 || !input_eligible(handle, windows[index].geometry.sequence)) return -1;
    *incarnation = windows[index].geometry.sequence;
    return 0;
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
    if (delivery_failed || !notification) return -1;
    int result = notification(message, notification_context);
    if (result < 0 || (result > 0 && message->type != MsgWindowCreateV2 &&
                      message->type != MsgWindowCreateV3)) delivery_failed = 1;
    return delivery_failed ? -1 : result;
}
static void remove_window(int index) {
    if (delivery_failed || !notification) return;
    tracked_window_t *window = &windows[index];
    window->retiring = 1; /* Exclude the retiring identity before observing replacement. */
    if (lease_mode) {
        if (lease_state.ever_anchored && lease_state.anchor_id == window->geometry.window_id &&
            lease_state.anchor_incarnation == window->geometry.sequence) {
            HWND foreground = GetForegroundWindow();
            uint64_t incarnation = 0;
            if (foreground_pair(foreground, &incarnation) != 0 ||
                av_lease_guest_transition(&lease_state, (uint64_t)(uintptr_t)foreground,
                    incarnation, &LeaseOps, NULL) != 0) { fail_native(); return; }
        }
    } else if (input_state.window_id == window->geometry.window_id &&
        input_state.incarnation == window->geometry.sequence &&
        av_input_reset(&input_state, &InputOps, NULL) != 0) { fail_native(); return; }
    av_message_t message = {.type = MsgWindowDestroy, .window_id = window->geometry.window_id,
                            .sequence = window->geometry.sequence};
    restore_window(window);
    memset(window, 0, sizeof(*window));
    emit(&message);
}
static void update_window(HWND handle) {
    if (delivery_failed || !notification) return;
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
        if (av_identity_next(&next_incarnation, &message.sequence) != 0) {
            delivery_failed = 1; return;
        }
        message.type = lease_mode ? MsgWindowCreateV3 : MsgWindowCreateV2;
        if (lease_mode) {
            int queued = emit(&message);
            if (queued != 0) return; /* Deferred capture admission has no input authority. */
            windows[index].handle = handle;
            windows[index].geometry = message; /* Commit only after Create was queued. */
            if (lease_state.phase == AvLeaseInitial &&
                av_lease_guest_begin(&lease_state, &LeaseOps, NULL) != 0) fail_native();
        } else {
            windows[index].handle = handle;
            windows[index].geometry = message;
            if (emit(&message) > 0) memset(&windows[index], 0, sizeof(windows[index]));
        }
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
static void append_observation(HWINEVENTHOOK hook, DWORD event, HWND handle, LONG object_id,
                               LONG child_id, DWORD thread_id, DWORD event_time) {
    if (observation_count == AvNativeObservations || observation_order == UINT64_MAX) {
        fail_native(); return;
    }
    int index = find_window(handle);
    native_observation_t *item = &observations[(observation_head + observation_count) % AvNativeObservations];
    *item = (native_observation_t){.hook = hook, .event = event, .handle = handle,
        .object_id = object_id, .child_id = child_id, .thread_id = thread_id,
        .event_time = event_time, .incarnation = index < 0 ? 0 : windows[index].geometry.sequence,
        .order = ++observation_order, .activation_operation = activation.active ? activation.operation : 0};
    ++observation_count;
}
static void CALLBACK window_event(HWINEVENTHOOK hook, DWORD event, HWND handle, LONG object_id,
                                  LONG child_id, DWORD thread_id, DWORD event_time) {
    if (stopping || !notification) return;
    if (lease_mode) {
        if (delivery_failed) return;
        if (callback_guard || GetCurrentThreadId() != owner_thread ||
            (event == EVENT_SYSTEM_FOREGROUND && (object_id != OBJID_WINDOW || child_id != CHILDID_SELF)) ||
            !((hook == foreground_hook && event == EVENT_SYSTEM_FOREGROUND) ||
              (hook == object_hook && event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_LOCATIONCHANGE) ||
              (hook == minimize_hook && event >= EVENT_SYSTEM_MINIMIZESTART && event <= EVENT_SYSTEM_MINIMIZEEND))) {
            fail_native(); return;
        }
        callback_guard = 1;
        if (event == EVENT_SYSTEM_FOREGROUND ||
            (handle && child_id == CHILDID_SELF &&
             (event < EVENT_OBJECT_CREATE || object_id == OBJID_WINDOW)))
            append_observation(hook, event, handle, object_id, child_id, thread_id, event_time);
        callback_guard = 0;
        return;
    }
    if (delivery_failed || !handle || child_id != CHILDID_SELF ||
        (event >= EVENT_OBJECT_CREATE && object_id != OBJID_WINDOW)) return;
    int index = find_window(handle);
    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) {
        if (index >= 0) remove_window(index);
    } else if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW ||
               event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_MINIMIZESTART ||
               event == EVENT_SYSTEM_MINIMIZEEND) update_window(handle);
}
static int observe_foreground(HWND handle, uint64_t incarnation, uint64_t expected_operation) {
    if (!lease_state.ever_anchored) return 0; /* Startup source never grants input. */
    uint64_t current_incarnation = 0;
    if (foreground_pair(handle, &current_incarnation) != 0 || incarnation != current_incarnation)
        return fail_native();
    if (activation.active && expected_operation == activation.operation &&
        (uint64_t)(uintptr_t)handle == activation.window_id && incarnation == activation.incarnation) {
        activation.seen_target = 1;
        return 0;
    }
    if (activation.active && (activation.seen_target ||
        (uint64_t)(uintptr_t)handle != lease_state.anchor_id ||
        incarnation != lease_state.anchor_incarnation)) return fail_native();
    return av_lease_guest_transition(&lease_state, (uint64_t)(uintptr_t)handle,
        incarnation, &LeaseOps, NULL);
}
static int reconcile_native(void *context) {
    (void)context;
    if (delivery_failed || !operation_guard || reconcile_guard || stopping ||
        GetCurrentThreadId() != owner_thread) return fail_native();
    reconcile_guard = 1;
    unsigned processed = 0;
    int result = 0;
    for (;;) {
        if (delivery_failed || input_desktop() != 0 || !lifetime_live() ||
            av_lease_guest_check(&lease_state, native_now(NULL)) != 0) { result = -1; break; }
        if (observation_count) {
            if (++processed > AvNativeObservations) { result = -1; break; }
            native_observation_t item = observations[observation_head];
            observation_head = (observation_head + 1) % AvNativeObservations;
            --observation_count;
            if (item.event == EVENT_SYSTEM_FOREGROUND) {
                if (observe_foreground(item.handle, item.incarnation, item.activation_operation) != 0) {
                    result = -1; break;
                }
            } else {
                int index = find_window(item.handle);
                if (index >= 0 && item.incarnation &&
                    windows[index].geometry.sequence != item.incarnation) { result = -1; break; }
                if (item.event == EVENT_OBJECT_DESTROY || item.event == EVENT_OBJECT_HIDE) {
                    if (index >= 0) {
                        if (!item.incarnation) { result = -1; break; }
                        remove_window(index);
                    }
                } else if (item.event == EVENT_OBJECT_CREATE || item.event == EVENT_OBJECT_SHOW ||
                    item.event == EVENT_OBJECT_LOCATIONCHANGE || item.event == EVENT_SYSTEM_MINIMIZESTART ||
                    item.event == EVENT_SYSTEM_MINIMIZEEND) update_window(item.handle);
            }
            continue;
        }
        if (lease_state.ever_anchored) {
            HWND foreground = GetForegroundWindow();
            uint64_t incarnation = 0;
            if (foreground_pair(foreground, &incarnation) != 0) { result = -1; break; }
            if (observation_count) continue;
            if (observe_foreground(foreground, incarnation, activation.active ? activation.operation : 0) != 0) {
                result = -1; break;
            }
        }
        if (!observation_count) break;
    }
    reconcile_guard = 0;
    return result != 0 || delivery_failed ? fail_native() : 0;
}
static BOOL CALLBACK enumerate_window(HWND handle, LPARAM context) {
    (void)context;
    update_window(handle);
    return delivery_failed ? FALSE : TRUE;
}
static int start_tracker(DWORD process_id, av_window_notify_t notify, void *context, int v3) {
    if (!process_id || !notify || notification || input_state.window_id ||
        lease_state.input.window_id || teardown_failed || operation_guard) return -1;
    previous_dpi_context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!previous_dpi_context) return -1;
    target_process = process_id;
    owner_thread = GetCurrentThreadId();
    notification = notify;
    notification_context = context;
    delivery_failed = 0; stopping = 0; lease_mode = v3;
    next_incarnation = observation_order = operation_order = last_clock = 0;
    observation_head = observation_count = 0;
    callback_guard = reconcile_guard = 0;
    memset(&input_state, 0, sizeof(input_state));
    memset(&lease_state, 0, sizeof(lease_state));
    memset(&activation, 0, sizeof(activation));
    if (owner_begin() != 0) { av_windows_stop(); return -1; }
    if (v3) {
        target_lifetime = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (!target_lifetime || !lifetime_live() || input_desktop() != 0) delivery_failed = 1;
    }
    if (!delivery_failed) {
        object_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
            window_event, process_id, 0, WINEVENT_OUTOFCONTEXT);
        minimize_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL,
            window_event, process_id, 0, WINEVENT_OUTOFCONTEXT);
        if (v3) foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, window_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!object_hook || !minimize_hook || (v3 && !foreground_hook)) delivery_failed = 1;
    }
    if (!delivery_failed && !EnumWindows(enumerate_window, 0)) delivery_failed = 1;
    int result = owner_end(delivery_failed ? -1 : 0, 1);
    if (result != 0) av_windows_stop();
    return result;
}
int av_windows_start(DWORD process_id, av_window_notify_t notify, void *context) {
    return start_tracker(process_id, notify, context, 0);
}
int av_windows_start_v3(DWORD process_id, av_window_notify_t notify, void *context) {
    return start_tracker(process_id, notify, context, 1);
}
int av_windows_stop(void) {
    if (owner_thread && GetCurrentThreadId() != owner_thread) { teardown_failed = 1; return fail_native(); }
    if (operation_guard) { teardown_failed = 1; return fail_native(); }
    stopping = 1;
    delivery_failed = 1; /* Disarm callbacks/publication before mandatory cleanup. */
    lease_state.phase = AvLeaseGuestTerminal; lease_state.deadline = 0;
    av_input_state_t *state = lease_mode ? &lease_state.input : &input_state;
    const av_input_ops_t *ops = lease_mode ? &LeaseOps.input : &InputOps;
    int released = av_input_reset(state, ops, NULL);
    if (released != 0) released = av_input_reset(state, ops, NULL);
    HWINEVENTHOOK *hooks[] = {&foreground_hook, &object_hook, &minimize_hook};
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        if (*hooks[i] && !UnhookWinEvent(*hooks[i])) { teardown_failed = 1; released = -1; }
        else *hooks[i] = NULL;
    }
    observation_head = observation_count = 0;
    memset(observations, 0, sizeof(observations));
    activation.active = 0;
    for (unsigned i = 0; i < AvMaxWindows; ++i) restore_window(&windows[i]);
    memset(windows, 0, sizeof(windows));
    notification = NULL; notification_context = NULL; target_process = 0;
    if (target_lifetime) {
        if (!CloseHandle(target_lifetime)) { teardown_failed = 1; released = -1; }
        target_lifetime = NULL;
    }
    if (previous_dpi_context) {
        if (!SetThreadDpiAwarenessContext(previous_dpi_context)) { teardown_failed = 1; released = -1; }
        previous_dpi_context = NULL;
    }
    if (!foreground_hook && !object_hook && !minimize_hook) owner_thread = 0;
    return teardown_failed ? -1 : released;
}
int av_windows_status(void) {
    return !target_process || !notification || delivery_failed || teardown_failed ? -1 : 0;
}
int av_windows_tick(void) {
    if (owner_begin() != 0) return -1;
    return owner_end(0, 1);
}
int av_windows_timeout(int maximum_ms) {
    if (owner_begin() != 0) return -1;
    int timeout = lease_mode ? av_lease_timeout(lease_state.deadline, native_now(NULL), maximum_ms)
                             : maximum_ms;
    if (timeout < 0 || (lease_mode && av_lease_guest_check(&lease_state, last_clock) != 0))
        return owner_end(-1, 0);
    return owner_end(0, 0) == 0 ? timeout : -1;
}
int av_windows_refresh(void) {
    if (owner_begin() != 0) return -1;
    if (lease_mode) {
        if (reconcile_native(NULL) != 0) return owner_end(-1, 0);
    } else if (input_state.window_id && GetForegroundWindow() != (HWND)(uintptr_t)input_state.window_id) {
        av_input_reset(&input_state, &InputOps, NULL);
        return owner_end(-1, 0); /* V2 foreground loss remains terminal. */
    }
    return owner_end(EnumWindows(enumerate_window, 0) ? 0 : -1, 1);
}
static int apply_native(const av_message_t *message, void *context) {
    tracked_window_t *window = context;
    HWND handle = (HWND)(uintptr_t)message->window_id;
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    if (process_id != target_process || !IsWindow(handle))
        return 0; /* Observed identity is no longer a live native target. */
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

int av_windows_apply(const av_message_t *message) {
    if (!message || owner_begin() != 0) return -1;
    if (lease_mode && reconcile_native(NULL) != 0) return owner_end(-1, 0);
    int index = find_window((HWND)(uintptr_t)message->window_id);
    tracked_window_t *window = index < 0 ? NULL : &windows[index];
    return owner_end(av_identity_apply(window ? &window->geometry : NULL, message,
        apply_native, window), 1);
}

static int legacy_input_target(void *context, uint64_t window_id, uint64_t incarnation, int activate) {
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
static int legacy_input_emit(void *context, const av_message_t *event) {
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
            if (legacy_input_target(NULL, event->window_id, event->sequence, 0) != 0) return -1;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = (DWORD)event->y;
        } else return -1;
    }
    return SendInput(1, &input, sizeof(input)) == 1 ? 0 : -1;
}
static const av_input_ops_t InputOps = {.target = legacy_input_target, .emit = legacy_input_emit};
/* Preflight may suspend only before insertion. It never drains observations or
 * resets held state; the lease core performs post-accounting reconciliation. */
static int lease_preflight(uint64_t window_id, uint64_t incarnation) {
    if (delivery_failed || !operation_guard || stopping || input_desktop() != 0 ||
        !input_eligible((HWND)(uintptr_t)window_id, incarnation) ||
        av_lease_guest_check(&lease_state, native_now(NULL)) != 0) return -1;
    if (delivery_failed) return -1;
    if (observation_count) return 1;
    HWND foreground = GetForegroundWindow();
    if (delivery_failed) return -1;
    if (foreground == (HWND)(uintptr_t)window_id) return observation_count ? 1 : 0;
    uint64_t foreground_incarnation = 0;
    if (foreground_pair(foreground, &foreground_incarnation) != 0 || delivery_failed) return -1;
    return 1;
}
static int lease_input_target(void *context, uint64_t window_id, uint64_t incarnation, int activate) {
    (void)context;
    HWND handle = (HWND)(uintptr_t)window_id;
    int index = find_window(handle);
    if (delivery_failed || !operation_guard) return -1;
    if (index < 0 || windows[index].retiring || windows[index].geometry.sequence != incarnation)
        return activate < 0 ? 1 : -1;
    if (activate < 0) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(handle, &process_id);
        if (!lifetime_live()) return -1;
        return process_id == target_process && IsWindow(handle) ? 0 : 1;
    } /* Identity probe never activates, releases or reconciles. */
    if (!activate) return lease_preflight(window_id, incarnation);
    uint64_t epoch = lease_state.epoch;
    if (reconcile_native(NULL) != 0) return -1;
    if (epoch != lease_state.epoch) return 1;
    if (input_desktop() != 0 || !input_eligible(handle, incarnation) || observation_count || delivery_failed) return -1;
    activation.active = 1; activation.seen_target = 0;
    activation.operation = operation_order;
    activation.window_id = window_id; activation.incarnation = incarnation;
    HWND foreground = GetForegroundWindow();
    if (observation_count || delivery_failed || (foreground != handle && !SetForegroundWindow(handle))) {
        activation.active = 0; return -1;
    }
    int result = reconcile_native(NULL);
    if (result == 0 && (input_desktop() != 0 || !input_eligible(handle, incarnation) ||
        GetForegroundWindow() != handle || observation_count || delivery_failed)) result = -1;
    activation.active = 0;
    return result;
}
static int lease_send(const av_message_t *event, INPUT *input, int cleanup, int partial) {
    if (!cleanup) {
        int result = lease_preflight(event->window_id, event->sequence);
        if (result != 0) return partial ? -1 : result;
        if (!lease_state.ever_anchored || lease_state.phase != AvLeaseActive ||
            lease_state.input.window_id != event->window_id ||
            lease_state.input.incarnation != event->sequence) return -1;
    }
    /* Return success even if SendInput reentered a callback: the caller must
     * first commit the successful held-bit change, then reconcile/clean up. */
    if (SendInput(1, input, sizeof(*input)) == 1) return 0;
    if (cleanup) operation_release_failed = 1;
    return -1;
}
static int lease_input_emit(void *context, const av_message_t *event) {
    (void)context;
    INPUT input = {0};
    int cleanup = !event->buffer_index && !event->flags &&
        (event->type == MsgInputKey || event->type == MsgInputButton);
    if (event->type == MsgInputKey) {
        uint16_t scan = av_input_scan_code(event->width);
        if (!scan) return -1;
        input.type = INPUT_KEYBOARD; input.ki.wScan = scan & 0xff;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0) |
            (event->flags ? 0 : KEYEVENTF_KEYUP);
    } else {
        input.type = INPUT_MOUSE;
        HWND handle = (HWND)(uintptr_t)event->window_id;
        if (event->type == MsgInputPointer) {
            av_message_t bounds;
            if (geometry(handle, &bounds) != 0 || event->x < 0 || event->y < 0 ||
                (uint32_t)event->x >= bounds.width || (uint32_t)event->y >= bounds.height) return -1;
            int64_t x = (int64_t)bounds.x + event->x, y = (int64_t)bounds.y + event->y;
            int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int64_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1 || x < left || y < top || x >= left + width || y >= top + height ||
                x < LONG_MIN || x > LONG_MAX || y < LONG_MIN || y > LONG_MAX) return -1;
            POINT point = {(LONG)x, (LONG)y};
            if (!point_owned(handle, point)) return -1;
            input.mi.dx = (LONG)((x - left) * 65535 / (width - 1));
            input.mi.dy = (LONG)((y - top) * 65535 / (height - 1));
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        } else if (event->type == MsgInputButton) {
            static const DWORD Down[4] = {0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
            static const DWORD Up[4] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
            if (event->width < 1 || event->width > 3) return -1;
            if (event->flags) {
                POINT point;
                if (!GetCursorPos(&point) || !point_owned(handle, point)) return -1;
            }
            input.mi.dwFlags = event->flags ? Down[event->width] : Up[event->width];
        } else if (event->type == MsgInputWheel) {
            POINT point;
            if (!GetCursorPos(&point) || !point_owned(handle, point)) return -1;
            if (event->x) {
                input.mi.dwFlags = MOUSEEVENTF_HWHEEL; input.mi.mouseData = (DWORD)event->x;
                int result = lease_send(event, &input, 0, 0);
                if (result != 0) return result;
            }
            if (!event->y) return 0;
            /* Every axis has its own pointer and authorization checks. Once
             * horizontal insertion succeeded, any vertical refusal is terminal. */
            if (!GetCursorPos(&point) || !point_owned(handle, point)) return -1;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL; input.mi.mouseData = (DWORD)event->y;
            return lease_send(event, &input, 0, event->x != 0);
        } else return -1;
    }
    return lease_send(event, &input, cleanup, 0);
}
static int lease_publish(void *context, const av_message_t *message) {
    (void)context;
    if (delivery_failed || !operation_guard || stopping || input_desktop() != 0 || !lifetime_live() ||
        observation_count) return fail_native();
    if (lease_state.ever_anchored &&
        lease_preflight(lease_state.anchor_id, lease_state.anchor_incarnation) != 0) return fail_native();
    av_message_t copy = *message;
    int result = emit(&copy);
    if (result != 0 || delivery_failed || observation_count) return fail_native();
    return 0;
}
static const av_lease_ops_t LeaseOps = {
    .input = {.target = lease_input_target, .emit = lease_input_emit},
    .publish = lease_publish, .reconcile = reconcile_native, .now_ms = native_now
};
int av_windows_input(const av_message_t *message) {
    if (!message || owner_begin() != 0) return -1;
    /* In particular do not reconcile here: stale epochs have strictly no native effects. */
    int result = lease_mode ? av_lease_guest_apply(&lease_state, message, &LeaseOps, NULL)
                            : av_input_apply(&input_state, message, &InputOps, NULL);
    return owner_end(result, 0);
}
