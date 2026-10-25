/**
 * @file window_state_adapter_windows.c
 * @brief Guest Win32 adapter executing window-state guest actions on one HWND (TODO #13).
 *
 * Contract: impl/seamless-interop/claude_45_percent/window_state_adapter_contract.md. The portable
 * logic in this file calls Win32 only through window_state_native_api_t. The real table exists under
 * _WIN32 and is the only code here that includes <windows.h>. Every other platform compiles the
 * portable logic against a test double.
 */
#include "window_state_adapter_windows.h"

#include <stdlib.h>
#include <string.h>

/* Portable copies of the Win32 values the adapter needs. The _WIN32 section asserts each one
 * against its windows.h macro, so a mismatch fails the Windows build. */
enum {
    WsaStyleCaption = 0x00C00000,
    WsaStyleThickFrame = 0x00040000,
    WsaStyleMaximizeBox = 0x00010000,
    WsaStyleMinimizeBox = 0x00020000,
    WsaSwMinimize = 6,
    WsaSwMaximize = 3,
    WsaSwRestore = 9,
    WsaShowNormal = 1,
    WsaShowMinimized = 2,
    WsaShowMaximized = 3,
    WsaSwpNoSize = 0x0001,
    WsaSwpNoMove = 0x0002,
    WsaSwpNoZOrder = 0x0004,
    WsaSwpNoActivate = 0x0010,
    WsaSwpFrameChanged = 0x0020,
    WsaWmClose = 0x0010
};

/** @brief One admitted window and its optional saved fullscreen state. */
typedef struct window_state_adapter_entry_t {
    /** @brief Nonzero while the entry binds an admitted window. */
    uint32_t in_use;
    /** @brief Nonzero while the saved fullscreen state below is present. */
    uint32_t fullscreen;
    /** @brief Protocol window identity bound at admission. */
    uint64_t window_id;
    /** @brief Protocol generation bound at admission. */
    uint32_t generation;
    /** @brief HWND value bound at admission. */
    uintptr_t window;
    /** @brief Owning process id captured at admission. */
    uint32_t process_id;
    /** @brief Owning thread id captured at admission. */
    uint32_t thread_id;
    /** @brief Style value saved by EnterFullscreen; valid while fullscreen is nonzero. */
    int64_t saved_style;
    /** @brief Window rectangle saved by EnterFullscreen. */
    window_state_rect_t saved_rect;
    /** @brief Placement saved by EnterFullscreen. */
    window_state_placement_t saved_placement;
} window_state_adapter_entry_t;

/** @brief Adapter object. Owns the table inline; freed only by window_state_adapter_destroy. */
struct window_state_adapter_t {
    /** @brief Native table copied at create. */
    window_state_native_api_t api;
    /** @brief Passed unchanged to every native entry. */
    void *context;
    /** @brief Thread id captured at create; the only thread allowed to use the adapter. */
    uint32_t owner_thread;
    /** @brief Nonzero while an entry is active on the owner thread. */
    uint32_t busy;
    /** @brief Native error of the most recent failed native call. */
    uint32_t last_error;
    /** @brief Bounded table of admitted windows. */
    window_state_adapter_entry_t entries[WaddleWindowStateAdapterMaxWindows];
};

/* ---- Argument and table helpers --------------------------------------------------------------- */

/** @brief True when every required table entry is present. */
static int api_complete(const window_state_native_api_t *api) {
    if (api->is_window == NULL) return 0;
    if (api->window_thread == NULL) return 0;
    if (api->is_iconic == NULL) return 0;
    if (api->is_zoomed == NULL) return 0;
    if (api->get_style == NULL) return 0;
    if (api->set_style == NULL) return 0;
    if (api->show_window == NULL) return 0;
    if (api->set_window_pos == NULL) return 0;
    if (api->get_placement == NULL) return 0;
    if (api->set_placement == NULL) return 0;
    if (api->get_rect == NULL) return 0;
    if (api->monitor_rect == NULL) return 0;
    if (api->foreground_window == NULL) return 0;
    if (api->set_foreground_window == NULL) return 0;
    if (api->set_active_window == NULL) return 0;
    if (api->attach_thread_input == NULL) return 0;
    if (api->post_message == NULL) return 0;
    if (api->current_thread_id == NULL) return 0;
    if (api->last_error == NULL) return 0;
    return 1;
}

/** @brief Records the native error of a failed call and returns NativeFailure. */
static window_state_adapter_status_t native_fail(window_state_adapter_t *adapter) {
    adapter->last_error = adapter->api.last_error(adapter->context);
    return WindowStateAdapterNativeFailure;
}

/**
 * @brief Owner-thread and re-entrancy gate of every entry that touches the adapter.
 *
 * Only current_thread_id runs before the owner check, so a foreign thread touches nothing else.
 */
static window_state_adapter_status_t enter(window_state_adapter_t *adapter) {
    if (adapter->api.current_thread_id(adapter->context) != adapter->owner_thread) {
        return WindowStateAdapterWrongThread;
    }
    if (adapter->busy != 0) return WindowStateAdapterReentrant;
    adapter->busy = 1;
    return WindowStateAdapterOk;
}

/** @brief Clears the busy flag set by enter. */
static void leave(window_state_adapter_t *adapter) {
    adapter->busy = 0;
}

/** @brief Finds the admitted entry for a pair, or NULL. */
static window_state_adapter_entry_t *find_pair(window_state_adapter_t *adapter, uint64_t window_id,
                                               uint32_t generation) {
    for (uint32_t index = 0; index < WaddleWindowStateAdapterMaxWindows; ++index) {
        window_state_adapter_entry_t *entry = &adapter->entries[index];
        if (entry->in_use != 0 && entry->window_id == window_id && entry->generation == generation) {
            return entry;
        }
    }
    return NULL;
}

/** @brief Finds the admitted entry for an HWND value, or NULL. */
static window_state_adapter_entry_t *find_window(window_state_adapter_t *adapter, uintptr_t window) {
    for (uint32_t index = 0; index < WaddleWindowStateAdapterMaxWindows; ++index) {
        window_state_adapter_entry_t *entry = &adapter->entries[index];
        if (entry->in_use != 0 && entry->window == window) return entry;
    }
    return NULL;
}

/** @brief Removes an entry and its saved state. The saved values are discarded, never applied. */
static void drop_entry(window_state_adapter_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));
}

/** @brief True when any entry holds a saved fullscreen state. */
static int any_fullscreen(const window_state_adapter_t *adapter) {
    for (uint32_t index = 0; index < WaddleWindowStateAdapterMaxWindows; ++index) {
        if (adapter->entries[index].in_use != 0 && adapter->entries[index].fullscreen != 0) return 1;
    }
    return 0;
}

/**
 * @brief Identity check before a native call that acts on the window.
 *
 * A window that no longer exists, or whose process or thread differs from the admitted pair, is
 * stale: the entry and its saved state are removed and StaleHandle is returned. A failing query is a
 * native failure and changes nothing.
 */
static window_state_adapter_status_t check_identity(window_state_adapter_t *adapter,
                                                    window_state_adapter_entry_t *entry) {
    uint32_t process_id = 0;
    uint32_t thread_id = 0;
    if (!adapter->api.is_window(adapter->context, entry->window)) {
        drop_entry(entry);
        return WindowStateAdapterStaleHandle;
    }
    if (!adapter->api.window_thread(adapter->context, entry->window, &process_id, &thread_id) || thread_id == 0) {
        return native_fail(adapter);
    }
    if (process_id != entry->process_id || thread_id != entry->thread_id) {
        drop_entry(entry);
        return WindowStateAdapterStaleHandle;
    }
    return WindowStateAdapterOk;
}

/* ---- Guest actions ---------------------------------------------------------------------------- */

/** @brief Minimize, Restore or Maximize: one ShowWindow call after the identity check. */
static window_state_adapter_status_t show_action(window_state_adapter_t *adapter,
                                                 window_state_adapter_entry_t *entry, int32_t command) {
    window_state_adapter_status_t status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (!adapter->api.show_window(adapter->context, entry->window, command)) return native_fail(adapter);
    return WindowStateAdapterOk;
}

/** @brief EnterFullscreen: save style, rectangle and placement, strip the frame, cover the monitor. */
static window_state_adapter_status_t enter_fullscreen(window_state_adapter_t *adapter,
                                                      window_state_adapter_entry_t *entry) {
    window_state_adapter_status_t status;
    int64_t style = 0;
    window_state_rect_t rect;
    window_state_rect_t monitor;
    window_state_placement_t placement;
    void *context = adapter->context;

    if (entry->fullscreen != 0) return WindowStateAdapterAlreadyFullscreen;
    status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (!adapter->api.get_style(context, entry->window, &style)) return native_fail(adapter);
    if (!adapter->api.get_rect(context, entry->window, &rect)) return native_fail(adapter);
    if (!adapter->api.get_placement(context, entry->window, &placement)) return native_fail(adapter);
    if (!adapter->api.monitor_rect(context, entry->window, &monitor)) return native_fail(adapter);
    if (!adapter->api.set_style(context, entry->window, style & ~(int64_t)(WsaStyleCaption | WsaStyleThickFrame))) {
        return native_fail(adapter);
    }
    if (!adapter->api.set_window_pos(context, entry->window, monitor.left, monitor.top,
                                     monitor.right - monitor.left, monitor.bottom - monitor.top,
                                     WsaSwpFrameChanged | WsaSwpNoZOrder | WsaSwpNoActivate)) {
        /* The first failure is the one reported. The rollback is best effort. When it also fails,
         * the saved state is kept so that LeaveFullscreen can undo the stripped style, and observe
         * keeps reporting the fullscreen state the window actually has. */
        status = native_fail(adapter);
        if (!adapter->api.set_style(context, entry->window, style)) {
            entry->saved_style = style;
            entry->saved_rect = rect;
            entry->saved_placement = placement;
            entry->fullscreen = 1;
        }
        return status;
    }
    entry->saved_style = style;
    entry->saved_rect = rect;
    entry->saved_placement = placement;
    entry->fullscreen = 1;
    return WindowStateAdapterOk;
}

/**
 * @brief LeaveFullscreen: restore the saved style and rectangle, then the saved normal rectangle.
 *
 * The placement is restored with the window's current minimized or maximized state as its show
 * command, not the saved one. The reducer emits its own restore and maximize actions before the
 * fullscreen transition, so re-applying the saved show command would undo them. Keeps the record on
 * failure, so a retry repeats the same calls.
 */
static window_state_adapter_status_t leave_fullscreen(window_state_adapter_t *adapter,
                                                      window_state_adapter_entry_t *entry) {
    window_state_adapter_status_t status;
    window_state_placement_t placement;
    void *context = adapter->context;
    int iconic = 0;
    int zoomed = 0;

    if (entry->fullscreen == 0) return WindowStateAdapterNoSavedState;
    status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (!adapter->api.set_style(context, entry->window, entry->saved_style)) return native_fail(adapter);
    if (!adapter->api.set_window_pos(context, entry->window, entry->saved_rect.left, entry->saved_rect.top,
                                     entry->saved_rect.right - entry->saved_rect.left,
                                     entry->saved_rect.bottom - entry->saved_rect.top,
                                     WsaSwpFrameChanged | WsaSwpNoZOrder | WsaSwpNoActivate)) {
        return native_fail(adapter);
    }
    if (!adapter->api.is_iconic(context, entry->window, &iconic)) return native_fail(adapter);
    if (!adapter->api.is_zoomed(context, entry->window, &zoomed)) return native_fail(adapter);
    placement = entry->saved_placement;
    placement.show_command = iconic != 0 ? WsaShowMinimized : (zoomed != 0 ? WsaShowMaximized : WsaShowNormal);
    if (!adapter->api.set_placement(context, entry->window, &placement)) return native_fail(adapter);
    entry->fullscreen = 0;
    return WindowStateAdapterOk;
}

/**
 * @brief Activate: SetForegroundWindow, with the AttachThreadInput fallback when it fails.
 *
 * The fallback attaches this thread to the foreground window's thread, retries the foreground and
 * active calls, and always detaches when it attached. The first failing call is reported.
 */
static window_state_adapter_status_t activate(window_state_adapter_t *adapter,
                                              window_state_adapter_entry_t *entry) {
    window_state_adapter_status_t status;
    void *context = adapter->context;
    uintptr_t foreground;
    uint32_t process_id = 0;
    uint32_t foreground_thread = 0;
    uint32_t self_thread;
    int attached = 0;
    int failed = 0;

    status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (adapter->api.set_foreground_window(context, entry->window)) return WindowStateAdapterOk;

    foreground = adapter->api.foreground_window(context);
    if (foreground == 0) return native_fail(adapter);
    if (!adapter->api.window_thread(context, foreground, &process_id, &foreground_thread) || foreground_thread == 0) {
        return native_fail(adapter);
    }
    self_thread = adapter->owner_thread;
    if (foreground_thread != self_thread) {
        if (!adapter->api.attach_thread_input(context, self_thread, foreground_thread, 1)) return native_fail(adapter);
        attached = 1;
    }
    if (!adapter->api.set_foreground_window(context, entry->window)) {
        status = native_fail(adapter);
        failed = 1;
    }
    if (!adapter->api.set_active_window(context, entry->window) && failed == 0) {
        status = native_fail(adapter);
        failed = 1;
    }
    if (attached != 0 && !adapter->api.attach_thread_input(context, self_thread, foreground_thread, 0) && failed == 0) {
        status = native_fail(adapter);
        failed = 1;
    }
    return failed != 0 ? status : WindowStateAdapterOk;
}

/** @brief SetGeometry: caller-prepared rectangle, no activation and no z-order change. */
static window_state_adapter_status_t set_geometry(window_state_adapter_t *adapter,
                                                  window_state_adapter_entry_t *entry,
                                                  const waddle_window_state_action_t *action) {
    window_state_adapter_status_t status;
    if (action->width == 0 || action->height == 0 || action->width > WaddleWindowStateAdapterMaxExtent ||
        action->height > WaddleWindowStateAdapterMaxExtent) {
        return WindowStateAdapterInvalidGeometry;
    }
    status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (!adapter->api.set_window_pos(adapter->context, entry->window, action->x, action->y,
                                     (int32_t)action->width, (int32_t)action->height,
                                     WsaSwpNoZOrder | WsaSwpNoActivate)) {
        return native_fail(adapter);
    }
    return WindowStateAdapterOk;
}

/** @brief PostClose: one WM_CLOSE post. Never destroys the window. */
static window_state_adapter_status_t post_close(window_state_adapter_t *adapter,
                                                window_state_adapter_entry_t *entry) {
    window_state_adapter_status_t status = check_identity(adapter, entry);
    if (status != WindowStateAdapterOk) return status;
    if (!adapter->api.post_message(adapter->context, entry->window, WsaWmClose, 0, 0)) return native_fail(adapter);
    return WindowStateAdapterOk;
}

/** @brief Dispatches one action. Argument and saved-state checks come before the identity check. */
static window_state_adapter_status_t run_action(window_state_adapter_t *adapter,
                                                window_state_adapter_entry_t *entry,
                                                const waddle_window_state_action_t *action) {
    switch (action->kind) {
    case WaddleWindowStateActionGuestMinimize:
        return show_action(adapter, entry, WsaSwMinimize);
    case WaddleWindowStateActionGuestRestore:
        return show_action(adapter, entry, WsaSwRestore);
    case WaddleWindowStateActionGuestMaximize:
        return show_action(adapter, entry, WsaSwMaximize);
    case WaddleWindowStateActionGuestEnterFullscreen:
        return enter_fullscreen(adapter, entry);
    case WaddleWindowStateActionGuestLeaveFullscreen:
        return leave_fullscreen(adapter, entry);
    case WaddleWindowStateActionGuestActivate:
        return activate(adapter, entry);
    case WaddleWindowStateActionGuestSetGeometry:
        return set_geometry(adapter, entry, action);
    case WaddleWindowStateActionGuestPostClose:
        return post_close(adapter, entry);
    case WaddleWindowStateActionHostSetMinimized:
    case WaddleWindowStateActionHostSetMaximized:
    case WaddleWindowStateActionHostUnsetMaximized:
    case WaddleWindowStateActionHostSetFullscreen:
    case WaddleWindowStateActionHostUnsetFullscreen:
    default:
        return WindowStateAdapterInvalidAction;
    }
}

/* ---- Public functions ------------------------------------------------------------------------- */

window_state_adapter_status_t window_state_adapter_create(const window_state_native_api_t *api, void *context,
                                                          window_state_adapter_t **out_adapter) {
    window_state_adapter_t *adapter;
    if (out_adapter == NULL) return WindowStateAdapterInvalidArgument;
    *out_adapter = NULL;
    if (api == NULL || !api_complete(api)) return WindowStateAdapterInvalidArgument;
    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) return WindowStateAdapterOutOfMemory;
    adapter->api = *api;
    adapter->context = context;
    adapter->owner_thread = adapter->api.current_thread_id(context);
    *out_adapter = adapter;
    return WindowStateAdapterOk;
}

window_state_adapter_status_t window_state_adapter_destroy(window_state_adapter_t **adapter_slot) {
    window_state_adapter_t *adapter;
    if (adapter_slot == NULL) return WindowStateAdapterInvalidArgument;
    adapter = *adapter_slot;
    if (adapter == NULL) return WindowStateAdapterOk;
    if (adapter->api.current_thread_id(adapter->context) != adapter->owner_thread) return WindowStateAdapterWrongThread;
    if (adapter->busy != 0) return WindowStateAdapterReentrant;
    if (any_fullscreen(adapter)) return WindowStateAdapterBusy;
    free(adapter);
    *adapter_slot = NULL;
    return WindowStateAdapterOk;
}

window_state_adapter_status_t window_state_adapter_admit(window_state_adapter_t *adapter, uintptr_t window,
                                                         uint64_t window_id, uint32_t generation) {
    window_state_adapter_status_t status;
    window_state_adapter_entry_t *slot = NULL;
    uint32_t process_id = 0;
    uint32_t thread_id = 0;
    if (adapter == NULL || window == 0 || window_id == 0 || generation == 0) return WindowStateAdapterInvalidArgument;
    status = enter(adapter);
    if (status != WindowStateAdapterOk) return status;

    if (find_pair(adapter, window_id, generation) != NULL || find_window(adapter, window) != NULL) {
        status = WindowStateAdapterAlreadyAdmitted;
    } else {
        for (uint32_t index = 0; index < WaddleWindowStateAdapterMaxWindows && slot == NULL; ++index) {
            if (adapter->entries[index].in_use == 0) slot = &adapter->entries[index];
        }
        if (slot == NULL) {
            status = WindowStateAdapterCapacityExhausted;
        } else if (!adapter->api.is_window(adapter->context, window)) {
            status = WindowStateAdapterNotFound;
        } else if (!adapter->api.window_thread(adapter->context, window, &process_id, &thread_id) || thread_id == 0) {
            status = native_fail(adapter);
        } else {
            memset(slot, 0, sizeof(*slot));
            slot->in_use = 1;
            slot->window_id = window_id;
            slot->generation = generation;
            slot->window = window;
            slot->process_id = process_id;
            slot->thread_id = thread_id;
            status = WindowStateAdapterOk;
        }
    }
    leave(adapter);
    return status;
}

window_state_adapter_status_t window_state_adapter_release(window_state_adapter_t *adapter, uint64_t window_id,
                                                           uint32_t generation) {
    window_state_adapter_status_t status;
    window_state_adapter_entry_t *entry;
    if (adapter == NULL || window_id == 0 || generation == 0) return WindowStateAdapterInvalidArgument;
    status = enter(adapter);
    if (status != WindowStateAdapterOk) return status;
    entry = find_pair(adapter, window_id, generation);
    if (entry == NULL) {
        status = WindowStateAdapterNotAdmitted;
    } else if (entry->fullscreen != 0) {
        status = WindowStateAdapterBusy;
    } else {
        drop_entry(entry);
        status = WindowStateAdapterOk;
    }
    leave(adapter);
    return status;
}

window_state_adapter_status_t window_state_adapter_observe(window_state_adapter_t *adapter, uintptr_t window,
                                                           uint64_t window_id, uint32_t generation,
                                                           uint32_t *out_guest_state) {
    window_state_adapter_status_t status;
    window_state_adapter_entry_t *entry;
    int iconic = 0;
    int zoomed = 0;
    int64_t style = 0;
    uint32_t bits = 0;
    if (adapter == NULL || out_guest_state == NULL || window_id == 0 || generation == 0) {
        return WindowStateAdapterInvalidArgument;
    }
    status = enter(adapter);
    if (status != WindowStateAdapterOk) return status;

    entry = find_pair(adapter, window_id, generation);
    if (entry == NULL) {
        status = WindowStateAdapterNotAdmitted;
    } else if (entry->window != window) {
        status = WindowStateAdapterInvalidArgument;
    } else {
        status = check_identity(adapter, entry);
        if (status == WindowStateAdapterOk) {
            if (!adapter->api.is_iconic(adapter->context, entry->window, &iconic)) {
                status = native_fail(adapter);
            } else if (!adapter->api.is_zoomed(adapter->context, entry->window, &zoomed)) {
                status = native_fail(adapter);
            } else if (entry->fullscreen == 0 &&
                       !adapter->api.get_style(adapter->context, entry->window, &style)) {
                status = native_fail(adapter);
            }
        }
        if (status == WindowStateAdapterOk) {
            /* While fullscreen, the stripped style has lost the frame, so the saved style is the truth. */
            if (entry->fullscreen != 0) style = entry->saved_style;
            if (iconic != 0) bits |= WaddleWindowStateMinimized;
            if (zoomed != 0) bits |= WaddleWindowStateMaximized;
            if (entry->fullscreen != 0) bits |= WaddleWindowStateFullscreen;
            if ((style & WsaStyleThickFrame) != 0) bits |= WaddleWindowStateResizable;
            if ((style & WsaStyleMaximizeBox) != 0) bits |= WaddleWindowStateMaximizable;
            if ((style & WsaStyleMinimizeBox) != 0) bits |= WaddleWindowStateMinimizable;
            *out_guest_state = bits;
        }
    }
    leave(adapter);
    return status;
}

window_state_adapter_status_t window_state_adapter_execute(window_state_adapter_t *adapter, uintptr_t window,
                                                           uint64_t window_id, uint32_t generation,
                                                           const waddle_window_state_action_t *actions,
                                                           uint64_t count, uint64_t *out_completed) {
    window_state_adapter_status_t status;
    window_state_adapter_entry_t *entry;
    uint64_t index;
    if (adapter == NULL || out_completed == NULL || window_id == 0 || generation == 0) {
        return WindowStateAdapterInvalidArgument;
    }
    if (count > WaddleWindowStateMaxActions || (count != 0 && actions == NULL)) {
        *out_completed = 0;
        return WindowStateAdapterInvalidArgument;
    }
    *out_completed = 0;
    status = enter(adapter);
    if (status != WindowStateAdapterOk) return status;

    entry = find_pair(adapter, window_id, generation);
    if (entry == NULL) {
        status = WindowStateAdapterNotAdmitted;
    } else if (entry->window != window) {
        status = WindowStateAdapterInvalidArgument;
    } else {
        for (index = 0; index < count; ++index) {
            status = run_action(adapter, entry, &actions[index]);
            if (status != WindowStateAdapterOk) {
                *out_completed = index;
                break;
            }
        }
        if (status == WindowStateAdapterOk) *out_completed = count;
    }
    leave(adapter);
    return status;
}

uint32_t window_state_adapter_last_error(const window_state_adapter_t *adapter) {
    if (adapter == NULL) return 0;
    if (adapter->api.current_thread_id(adapter->context) != adapter->owner_thread) return 0;
    return adapter->last_error;
}

/* ---- Win32 native table ----------------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>

_Static_assert(WsaStyleCaption == WS_CAPTION, "WS_CAPTION value");
_Static_assert(WsaStyleThickFrame == WS_THICKFRAME, "WS_THICKFRAME value");
_Static_assert(WsaStyleMaximizeBox == WS_MAXIMIZEBOX, "WS_MAXIMIZEBOX value");
_Static_assert(WsaStyleMinimizeBox == WS_MINIMIZEBOX, "WS_MINIMIZEBOX value");
_Static_assert(WsaSwMinimize == SW_MINIMIZE, "SW_MINIMIZE value");
_Static_assert(WsaSwMaximize == SW_MAXIMIZE, "SW_MAXIMIZE value");
_Static_assert(WsaSwRestore == SW_RESTORE, "SW_RESTORE value");
_Static_assert(WsaShowNormal == SW_SHOWNORMAL, "SW_SHOWNORMAL value");
_Static_assert(WsaShowMinimized == SW_SHOWMINIMIZED, "SW_SHOWMINIMIZED value");
_Static_assert(WsaShowMaximized == SW_SHOWMAXIMIZED, "SW_SHOWMAXIMIZED value");
_Static_assert(WsaSwpNoSize == SWP_NOSIZE, "SWP_NOSIZE value");
_Static_assert(WsaSwpNoMove == SWP_NOMOVE, "SWP_NOMOVE value");
_Static_assert(WsaSwpNoZOrder == SWP_NOZORDER, "SWP_NOZORDER value");
_Static_assert(WsaSwpNoActivate == SWP_NOACTIVATE, "SWP_NOACTIVATE value");
_Static_assert(WsaSwpFrameChanged == SWP_FRAMECHANGED, "SWP_FRAMECHANGED value");
_Static_assert(WsaWmClose == WM_CLOSE, "WM_CLOSE value");

static int nat_is_window(void *context, uintptr_t window) {
    (void)context;
    return IsWindow((HWND)window) ? 1 : 0;
}

static int nat_window_thread(void *context, uintptr_t window, uint32_t *process_id, uint32_t *thread_id) {
    DWORD process = 0;
    DWORD thread;
    (void)context;
    thread = GetWindowThreadProcessId((HWND)window, &process);
    if (thread == 0) return 0;
    *process_id = (uint32_t)process;
    *thread_id = (uint32_t)thread;
    return 1;
}

static int nat_is_iconic(void *context, uintptr_t window, int *iconic) {
    (void)context;
    *iconic = IsIconic((HWND)window) ? 1 : 0;
    return 1;
}

static int nat_is_zoomed(void *context, uintptr_t window, int *zoomed) {
    (void)context;
    *zoomed = IsZoomed((HWND)window) ? 1 : 0;
    return 1;
}

static int nat_get_style(void *context, uintptr_t window, int64_t *style) {
    LONG_PTR value;
    (void)context;
    SetLastError(0);
    value = GetWindowLongPtrW((HWND)window, GWL_STYLE);
    if (value == 0 && GetLastError() != 0) return 0;
    *style = (int64_t)(uint32_t)(DWORD)value;
    return 1;
}

static int nat_set_style(void *context, uintptr_t window, int64_t style) {
    LONG_PTR previous;
    (void)context;
    SetLastError(0);
    previous = SetWindowLongPtrW((HWND)window, GWL_STYLE, (LONG_PTR)(LONG)(DWORD)(uint32_t)style);
    if (previous == 0 && GetLastError() != 0) return 0;
    return 1;
}

static int nat_show_window(void *context, uintptr_t window, int32_t command) {
    (void)context;
    /* ShowWindow reports only prior visibility, not failure. The identity check precedes it, and the
     * observed state is what the smoke and the reducer verify. */
    (void)ShowWindow((HWND)window, (int)command);
    return 1;
}

static int nat_set_window_pos(void *context, uintptr_t window, int32_t x, int32_t y, int32_t width,
                              int32_t height, uint32_t flags) {
    (void)context;
    return SetWindowPos((HWND)window, NULL, x, y, width, height, (UINT)flags) ? 1 : 0;
}

static int nat_get_placement(void *context, uintptr_t window, window_state_placement_t *placement) {
    WINDOWPLACEMENT value;
    (void)context;
    memset(&value, 0, sizeof(value));
    value.length = sizeof(value);
    if (!GetWindowPlacement((HWND)window, &value)) return 0;
    placement->flags = (uint32_t)value.flags;
    placement->show_command = (uint32_t)value.showCmd;
    placement->min_x = value.ptMinPosition.x;
    placement->min_y = value.ptMinPosition.y;
    placement->max_x = value.ptMaxPosition.x;
    placement->max_y = value.ptMaxPosition.y;
    placement->normal.left = value.rcNormalPosition.left;
    placement->normal.top = value.rcNormalPosition.top;
    placement->normal.right = value.rcNormalPosition.right;
    placement->normal.bottom = value.rcNormalPosition.bottom;
    return 1;
}

static int nat_set_placement(void *context, uintptr_t window, const window_state_placement_t *placement) {
    WINDOWPLACEMENT value;
    (void)context;
    memset(&value, 0, sizeof(value));
    value.length = sizeof(value);
    value.flags = placement->flags;
    value.showCmd = placement->show_command;
    value.ptMinPosition.x = placement->min_x;
    value.ptMinPosition.y = placement->min_y;
    value.ptMaxPosition.x = placement->max_x;
    value.ptMaxPosition.y = placement->max_y;
    value.rcNormalPosition.left = placement->normal.left;
    value.rcNormalPosition.top = placement->normal.top;
    value.rcNormalPosition.right = placement->normal.right;
    value.rcNormalPosition.bottom = placement->normal.bottom;
    return SetWindowPlacement((HWND)window, &value) ? 1 : 0;
}

static int nat_get_rect(void *context, uintptr_t window, window_state_rect_t *rect) {
    RECT value;
    (void)context;
    if (!GetWindowRect((HWND)window, &value)) return 0;
    rect->left = value.left;
    rect->top = value.top;
    rect->right = value.right;
    rect->bottom = value.bottom;
    return 1;
}

static int nat_monitor_rect(void *context, uintptr_t window, window_state_rect_t *rect) {
    HMONITOR monitor;
    MONITORINFO info;
    (void)context;
    monitor = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (monitor == NULL || !GetMonitorInfoW(monitor, &info)) return 0;
    rect->left = info.rcMonitor.left;
    rect->top = info.rcMonitor.top;
    rect->right = info.rcMonitor.right;
    rect->bottom = info.rcMonitor.bottom;
    return 1;
}

static uintptr_t nat_foreground_window(void *context) {
    (void)context;
    return (uintptr_t)GetForegroundWindow();
}

static int nat_set_foreground_window(void *context, uintptr_t window) {
    (void)context;
    return SetForegroundWindow((HWND)window) ? 1 : 0;
}

static int nat_set_active_window(void *context, uintptr_t window) {
    (void)context;
    return SetActiveWindow((HWND)window) != NULL ? 1 : 0;
}

static int nat_attach_thread_input(void *context, uint32_t from_thread, uint32_t to_thread, int attach) {
    (void)context;
    return AttachThreadInput((DWORD)from_thread, (DWORD)to_thread, attach ? TRUE : FALSE) ? 1 : 0;
}

static int nat_post_message(void *context, uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    (void)context;
    return PostMessageW((HWND)window, (UINT)message, (WPARAM)wparam, (LPARAM)lparam) ? 1 : 0;
}

static uint32_t nat_current_thread_id(void *context) {
    (void)context;
    return (uint32_t)GetCurrentThreadId();
}

static uint32_t nat_last_error(void *context) {
    (void)context;
    return (uint32_t)GetLastError();
}

static const window_state_native_api_t NativeWindowStateApi = {
    .is_window = nat_is_window,
    .window_thread = nat_window_thread,
    .is_iconic = nat_is_iconic,
    .is_zoomed = nat_is_zoomed,
    .get_style = nat_get_style,
    .set_style = nat_set_style,
    .show_window = nat_show_window,
    .set_window_pos = nat_set_window_pos,
    .get_placement = nat_get_placement,
    .set_placement = nat_set_placement,
    .get_rect = nat_get_rect,
    .monitor_rect = nat_monitor_rect,
    .foreground_window = nat_foreground_window,
    .set_foreground_window = nat_set_foreground_window,
    .set_active_window = nat_set_active_window,
    .attach_thread_input = nat_attach_thread_input,
    .post_message = nat_post_message,
    .current_thread_id = nat_current_thread_id,
    .last_error = nat_last_error,
};

const window_state_native_api_t *window_state_native_api(void) {
    return &NativeWindowStateApi;
}
#else
const window_state_native_api_t *window_state_native_api(void) {
    return NULL;
}
#endif
