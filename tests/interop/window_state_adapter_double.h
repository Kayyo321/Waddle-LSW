/**
 * @file window_state_adapter_double.h
 * @brief Deterministic in-memory Win32 double for window_state_native_api_t (test only).
 *
 * The double models only what the adapter depends on: window existence, owning process and thread,
 * style, rectangle, placement, minimized and maximized flags, the monitor rectangle, the foreground
 * window with an optional foreground lock that AttachThreadInput clears, and the posted close.
 * Every table entry records its call in a log, except current_thread_id and last_error. Any call can
 * be made to fail, either the nth call of one entry or each call with a per-mille probability from a
 * fixed xorshift stream. A failing call has no effect on the double and sets the native error.
 *
 * The double never destroys a window on its own. Tests destroy or reuse windows explicitly.
 */
#ifndef WindowStateAdapterDoubleH
#define WindowStateAdapterDoubleH

#include "window_state_adapter_windows.h"

#include <stdint.h>
#include <string.h>

/** @brief Entries of the double that a test can address. */
typedef enum wsa_double_entry_t {
    WsaDoubleIsWindow = 0,
    WsaDoubleWindowThread,
    WsaDoubleIsIconic,
    WsaDoubleIsZoomed,
    WsaDoubleGetStyle,
    WsaDoubleSetStyle,
    WsaDoubleShowWindow,
    WsaDoubleSetWindowPos,
    WsaDoubleGetPlacement,
    WsaDoubleSetPlacement,
    WsaDoubleGetRect,
    WsaDoubleMonitorRect,
    WsaDoubleForegroundWindow,
    WsaDoubleSetForegroundWindow,
    WsaDoubleSetActiveWindow,
    WsaDoubleAttachThreadInput,
    WsaDoublePostMessage,
    WsaDoubleCurrentThreadId,
    WsaDoubleLastError,
    WsaDoubleEntryCount
} wsa_double_entry_t;

/** @brief Fixed style bits of a created window: caption, thick frame and both boxes. */
#define WsaDoubleDefaultStyle (WsaFrameMaskCaption | WsaFrameMaskThickFrame | WsaFrameMaskMaximizeBox | WsaFrameMaskMinimizeBox)
/** @brief Style bits the adapter strips for fullscreen and reads for capabilities. */
#define WsaFrameMaskCaption 0x00C00000LL
#define WsaFrameMaskThickFrame 0x00040000LL
#define WsaFrameMaskMaximizeBox 0x00010000LL
#define WsaFrameMaskMinimizeBox 0x00020000LL
/** @brief Number of double windows. */
#define WsaDoubleMaxWindows 256
/** @brief Capacity of the call log. */
#define WsaDoubleLogCapacity 1024

/** @brief One simulated top-level window. */
typedef struct wsa_double_window_t {
    /** @brief HWND value; 0 when the slot is unused. */
    uintptr_t window;
    /** @brief Nonzero while the window exists. */
    int exists;
    /** @brief Owning process id. */
    uint32_t process_id;
    /** @brief Owning thread id. */
    uint32_t thread_id;
    /** @brief GWL_STYLE value. */
    int64_t style;
    /** @brief Window rectangle. */
    window_state_rect_t rect;
    /** @brief Placement, showCmd 1 normal, 2 minimized, 3 maximized. */
    window_state_placement_t placement;
    /** @brief Nonzero while minimized. */
    int iconic;
    /** @brief Nonzero while maximized. */
    int zoomed;
    /** @brief Nonzero when a minimized window restores to maximized (it was maximized when minimized). */
    int restore_to_maximized;
    /** @brief WM_CLOSE posts received. */
    uint32_t close_posts;
} wsa_double_window_t;

/** @brief Complete double state. Initialize with wsa_double_init. */
typedef struct wsa_double_t {
    /** @brief Simulated windows. */
    wsa_double_window_t windows[WsaDoubleMaxWindows];
    /** @brief Monitor rectangle returned by monitor_rect. */
    window_state_rect_t monitor;
    /** @brief Current foreground window, or 0. */
    uintptr_t foreground;
    /** @brief Nonzero when SetForegroundWindow is refused unless this thread is attached. */
    uint32_t foreground_locked;
    /** @brief Nonzero while this thread is attached to the foreground window's thread. */
    uint32_t attached;
    /** @brief Thread id returned by current_thread_id. Tests change it to model a foreign thread. */
    uint32_t current_thread;
    /** @brief Native error of the most recent failing call. */
    uint32_t native_error;
    /** @brief Error value a failing call reports. */
    uint32_t error_value;
    /** @brief Per entry: one-based call number that fails; 0 when that entry is not armed. */
    uint32_t fail_at[WsaDoubleEntryCount];
    /** @brief Per entry: nonzero when every call from fail_at onward fails, not only that call. */
    uint32_t fail_forever[WsaDoubleEntryCount];
    /** @brief Calls per entry since the last arm. */
    uint32_t calls[WsaDoubleEntryCount];
    /** @brief Per-mille probability that any faultable call fails; 0 disables random faults. */
    uint32_t fault_per_mille;
    /** @brief xorshift64 state of the random faults. */
    uint64_t rng;
    /** @brief Recorded calls, in order. */
    wsa_double_entry_t log[WsaDoubleLogCapacity];
    /** @brief Number of recorded calls. */
    uint32_t log_count;
} wsa_double_t;

/** @brief Initializes the double with a 1920 by 1080 monitor, no windows and the given current thread. */
static inline void wsa_double_init(wsa_double_t *d, uint32_t current_thread) {
    memset(d, 0, sizeof(*d));
    d->monitor.left = 0;
    d->monitor.top = 0;
    d->monitor.right = 1920;
    d->monitor.bottom = 1080;
    d->current_thread = current_thread;
    d->error_value = 5u;
    d->rng = UINT64_C(0x9e3779b97f4a7c15);
}

/** @brief Clears the call counts and the call log. Failure arming is unchanged. */
static inline void wsa_double_reset(wsa_double_t *d) {
    memset(d->calls, 0, sizeof(d->calls));
    d->log_count = 0;
}

/**
 * @brief Arms a deterministic failure for one entry: its nth call from now fails, and every later call
 *        fails too when forever is nonzero. "From now" means after the calls already counted, so the
 *        count is relative to the calls made before this arm. Other entries keep their arming.
 */
static inline void wsa_double_arm(wsa_double_t *d, wsa_double_entry_t entry, uint32_t nth, uint32_t forever) {
    d->fail_at[entry] = d->calls[entry] + nth;
    d->fail_forever[entry] = forever;
}

/** @brief Disarms every deterministic and random failure. */
static inline void wsa_double_disarm(wsa_double_t *d) {
    memset(d->fail_at, 0, sizeof(d->fail_at));
    memset(d->fail_forever, 0, sizeof(d->fail_forever));
    d->fault_per_mille = 0;
}

/** @brief Adds a visible, normal-sized window and returns its record. */
static wsa_double_window_t *wsa_double_add(wsa_double_t *d, uintptr_t window, uint32_t process_id,
                                           uint32_t thread_id, window_state_rect_t rect) {
    for (uint32_t index = 0; index < WsaDoubleMaxWindows; ++index) {
        wsa_double_window_t *record = &d->windows[index];
        if (record->window == 0) {
            memset(record, 0, sizeof(*record));
            record->window = window;
            record->exists = 1;
            record->process_id = process_id;
            record->thread_id = thread_id;
            record->style = WsaDoubleDefaultStyle;
            record->rect = rect;
            record->placement.flags = 0;
            record->placement.show_command = 1u;
            record->placement.normal = rect;
            return record;
        }
    }
    return NULL;
}

/** @brief Finds a record by HWND value, existing or not. */
static inline wsa_double_window_t *wsa_double_find(wsa_double_t *d, uintptr_t window) {
    for (uint32_t index = 0; index < WsaDoubleMaxWindows; ++index) {
        if (d->windows[index].window == window && window != 0) return &d->windows[index];
    }
    return NULL;
}

/** @brief Read-only record lookup by HWND value for test assertions. */
static inline const wsa_double_window_t *wsa_double_record(const wsa_double_t *d, uintptr_t window) {
    for (uint32_t index = 0; index < WsaDoubleMaxWindows; ++index) {
        if (d->windows[index].window == window && window != 0) return &d->windows[index];
    }
    return NULL;
}

/** @brief Destroys a window from outside the adapter, as the application would. */
static inline void wsa_double_destroy_window(wsa_double_t *d, uintptr_t window) {
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (record != NULL) {
        record->exists = 0;
        if (d->foreground == window) d->foreground = 0;
    }
}

/** @brief Reuses an HWND value for a new window with a different owner, as Windows may. */
static inline void wsa_double_reuse(wsa_double_t *d, uintptr_t window, uint32_t process_id, uint32_t thread_id) {
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (record != NULL) {
        record->exists = 1;
        record->process_id = process_id;
        record->thread_id = thread_id;
    }
}

/** @brief Deterministic per-call draw for random faults. */
static inline uint32_t wsa_double_draw(wsa_double_t *d) {
    uint64_t value = d->rng;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    d->rng = value;
    return (uint32_t)(value % 1000u);
}

/**
 * @brief Records one call and decides whether it fails. A failing call sets native_error and must
 *        have no effect.
 */
static inline int wsa_double_begin(wsa_double_t *d, wsa_double_entry_t entry, int faultable) {
    int fail = 0;
    d->calls[entry] += 1u;
    if (entry != WsaDoubleCurrentThreadId && entry != WsaDoubleLastError) {
        if (d->log_count < WsaDoubleLogCapacity) d->log[d->log_count++] = entry;
    }
    if (d->fail_at[entry] != 0u &&
        (d->calls[entry] == d->fail_at[entry] || (d->fail_forever[entry] != 0u && d->calls[entry] >= d->fail_at[entry]))) {
        fail = 1;
    }
    if (!fail && faultable != 0 && d->fault_per_mille != 0u && wsa_double_draw(d) < d->fault_per_mille) {
        fail = 1;
    }
    if (fail) d->native_error = d->error_value;
    return fail ? 0 : 1;
}

static inline int wsa_double_is_window(void *context, uintptr_t window) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record;
    if (!wsa_double_begin(d, WsaDoubleIsWindow, 1)) return 0;
    record = wsa_double_find(d, window);
    return record != NULL && record->exists != 0;
}

static inline int wsa_double_window_thread(void *context, uintptr_t window, uint32_t *process_id, uint32_t *thread_id) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleWindowThread, 1)) return 0;
    if (record == NULL || record->exists == 0) return 0;
    *process_id = record->process_id;
    *thread_id = record->thread_id;
    return 1;
}

static inline int wsa_double_is_iconic(void *context, uintptr_t window, int *iconic) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleIsIconic, 1)) return 0;
    *iconic = record != NULL && record->iconic != 0 ? 1 : 0;
    return 1;
}

static inline int wsa_double_is_zoomed(void *context, uintptr_t window, int *zoomed) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleIsZoomed, 1)) return 0;
    *zoomed = record != NULL && record->zoomed != 0 ? 1 : 0;
    return 1;
}

static inline int wsa_double_get_style(void *context, uintptr_t window, int64_t *style) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleGetStyle, 1)) return 0;
    if (record == NULL) return 0;
    *style = record->style;
    return 1;
}

static inline int wsa_double_set_style(void *context, uintptr_t window, int64_t style) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleSetStyle, 1)) return 0;
    if (record == NULL) return 0;
    record->style = style;
    return 1;
}

static inline int wsa_double_show_window(void *context, uintptr_t window, int32_t command) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleShowWindow, 1)) return 0;
    if (record == NULL) return 1;
    if (command == 6) { /* SW_MINIMIZE */
        record->restore_to_maximized = record->zoomed;
        record->iconic = 1;
        record->placement.show_command = 2u;
    } else if (command == 3) { /* SW_MAXIMIZE: a minimized window is restored and maximized */
        record->iconic = 0;
        record->zoomed = 1;
        record->placement.show_command = 3u;
    } else if (command == 9) { /* SW_RESTORE */
        if (record->iconic != 0) {
            record->zoomed = record->restore_to_maximized;
        } else {
            record->zoomed = 0;
        }
        record->iconic = 0;
        record->placement.show_command = record->zoomed != 0 ? 3u : 1u;
    }
    return 1;
}

static int wsa_double_set_window_pos(void *context, uintptr_t window, int32_t x, int32_t y, int32_t width,
                                     int32_t height, uint32_t flags) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleSetWindowPos, 1)) return 0;
    if (record == NULL) return 0;
    if ((flags & 0x0002u) == 0u) { /* not SWP_NOMOVE */
        record->rect.left = x;
        record->rect.top = y;
    }
    if ((flags & 0x0001u) == 0u) { /* not SWP_NOSIZE */
        record->rect.right = x + width;
        record->rect.bottom = y + height;
    }
    return 1;
}

static inline int wsa_double_get_placement(void *context, uintptr_t window, window_state_placement_t *placement) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleGetPlacement, 1)) return 0;
    if (record == NULL) return 0;
    *placement = record->placement;
    return 1;
}

static inline int wsa_double_set_placement(void *context, uintptr_t window, const window_state_placement_t *placement) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleSetPlacement, 1)) return 0;
    if (record == NULL) return 0;
    record->placement = *placement;
    record->iconic = placement->show_command == 2u ? 1 : 0;
    record->zoomed = placement->show_command == 3u ? 1 : 0;
    if (placement->show_command == 1u) record->rect = placement->normal;
    return 1;
}

static inline int wsa_double_get_rect(void *context, uintptr_t window, window_state_rect_t *rect) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleGetRect, 1)) return 0;
    if (record == NULL) return 0;
    *rect = record->rect;
    return 1;
}

static inline int wsa_double_monitor_rect(void *context, uintptr_t window, window_state_rect_t *rect) {
    wsa_double_t *d = (wsa_double_t *)context;
    if (!wsa_double_begin(d, WsaDoubleMonitorRect, 1)) return 0;
    if (wsa_double_find(d, window) == NULL) return 0;
    *rect = d->monitor;
    return 1;
}

static inline uintptr_t wsa_double_foreground_window(void *context) {
    wsa_double_t *d = (wsa_double_t *)context;
    if (!wsa_double_begin(d, WsaDoubleForegroundWindow, 1)) return 0;
    return d->foreground;
}

static inline int wsa_double_set_foreground_window(void *context, uintptr_t window) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleSetForegroundWindow, 1)) return 0;
    if (record == NULL || record->exists == 0) return 0;
    if (d->foreground_locked != 0 && d->attached == 0) {
        d->native_error = d->error_value;
        return 0;
    }
    d->foreground = window;
    return 1;
}

static inline int wsa_double_set_active_window(void *context, uintptr_t window) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    if (!wsa_double_begin(d, WsaDoubleSetActiveWindow, 1)) return 0;
    return record != NULL && record->exists != 0 ? 1 : 0;
}

static inline int wsa_double_attach_thread_input(void *context, uint32_t from_thread, uint32_t to_thread, int attach) {
    wsa_double_t *d = (wsa_double_t *)context;
    if (!wsa_double_begin(d, WsaDoubleAttachThreadInput, 1)) return 0;
    if (from_thread == 0u || to_thread == 0u) return 0;
    d->attached = attach != 0 ? 1u : 0u;
    return 1;
}

static inline int wsa_double_post_message(void *context, uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    wsa_double_t *d = (wsa_double_t *)context;
    wsa_double_window_t *record = wsa_double_find(d, window);
    (void)wparam;
    (void)lparam;
    if (!wsa_double_begin(d, WsaDoublePostMessage, 1)) return 0;
    if (record == NULL || record->exists == 0) return 0;
    if (message == 0x0010u) record->close_posts += 1u; /* WM_CLOSE */
    return 1;
}

static inline uint32_t wsa_double_current_thread_id(void *context) {
    wsa_double_t *d = (wsa_double_t *)context;
    (void)wsa_double_begin(d, WsaDoubleCurrentThreadId, 0);
    return d->current_thread;
}

static inline uint32_t wsa_double_last_error(void *context) {
    wsa_double_t *d = (wsa_double_t *)context;
    (void)wsa_double_begin(d, WsaDoubleLastError, 0);
    return d->native_error;
}

/** @brief The double's native table. Its context is a wsa_double_t pointer. */
static const window_state_native_api_t WsaDoubleApi = {
    .is_window = wsa_double_is_window,
    .window_thread = wsa_double_window_thread,
    .is_iconic = wsa_double_is_iconic,
    .is_zoomed = wsa_double_is_zoomed,
    .get_style = wsa_double_get_style,
    .set_style = wsa_double_set_style,
    .show_window = wsa_double_show_window,
    .set_window_pos = wsa_double_set_window_pos,
    .get_placement = wsa_double_get_placement,
    .set_placement = wsa_double_set_placement,
    .get_rect = wsa_double_get_rect,
    .monitor_rect = wsa_double_monitor_rect,
    .foreground_window = wsa_double_foreground_window,
    .set_foreground_window = wsa_double_set_foreground_window,
    .set_active_window = wsa_double_set_active_window,
    .attach_thread_input = wsa_double_attach_thread_input,
    .post_message = wsa_double_post_message,
    .current_thread_id = wsa_double_current_thread_id,
    .last_error = wsa_double_last_error,
};

/** @brief Returns the double's table. */
static const window_state_native_api_t *wsa_double_api(void) {
    return &WsaDoubleApi;
}

#endif
