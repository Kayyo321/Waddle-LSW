/**
 * @file window_state_adapter_windows.h
 * @brief Guest Win32 adapter that executes window-state guest actions on one HWND (TODO #13).
 *
 * The adapter executes the reducer's guest actions (waddle_window_state_action_t values from
 * window_state.h) in order on an admitted top-level window, and produces the guest observation
 * (state and capability bits of window_state.h) from native facts. Contract:
 * impl/seamless-interop/claude_45_percent/window_state_adapter_contract.md.
 *
 * No Win32 type appears in this header. Window handles travel as uintptr_t. The real Win32
 * implementation is returned by window_state_native_api() under _WIN32 only; every other
 * platform gets NULL and must supply a native table (the test double does).
 *
 * Threading: the owner thread is the thread that calls window_state_adapter_create. Every other
 * function must run on that thread. A call from another thread returns WrongThread and touches
 * nothing. A call made while another entry of the same adapter is active (including one reached
 * through a message dispatched by a native call) returns Reentrant. WinEvent callbacks must not
 * call the adapter.
 */
#ifndef WaddleWindowStateAdapterWindowsH
#define WaddleWindowStateAdapterWindowsH

#include <stddef.h>
#include <stdint.h>

#include "window_state.h"

/** @brief Maximum number of admitted windows, and of saved fullscreen states. */
#define WaddleWindowStateAdapterMaxWindows 64

/** @brief Largest accepted SetGeometry extent in physical pixels. Mirrors the reducer MaxExtent. */
#define WaddleWindowStateAdapterMaxExtent 16384

/** @brief Result code of every adapter function. Numeric values are frozen. */
typedef enum window_state_adapter_status_t {
    /** @brief The operation completed. */
    WindowStateAdapterOk = 0,
    /** @brief A required pointer is NULL, or an identity or count argument is invalid. */
    WindowStateAdapterInvalidArgument = 1,
    /** @brief Called from a thread other than the owner. Nothing was touched. */
    WindowStateAdapterWrongThread = 2,
    /** @brief Called while another entry of this adapter was active. Nothing was touched. */
    WindowStateAdapterReentrant = 3,
    /** @brief create could not allocate the adapter. The output is NULL. */
    WindowStateAdapterOutOfMemory = 4,
    /** @brief admit found no free entry among WaddleWindowStateAdapterMaxWindows. */
    WindowStateAdapterCapacityExhausted = 5,
    /** @brief The (window_id, generation) pair is not admitted, or it was removed as stale. */
    WindowStateAdapterNotAdmitted = 6,
    /** @brief admit found the pair or the HWND already bound. */
    WindowStateAdapterAlreadyAdmitted = 7,
    /** @brief admit found that the HWND does not exist. */
    WindowStateAdapterNotFound = 8,
    /** @brief The HWND no longer exists, or its process or thread identity differs from the admitted one. */
    WindowStateAdapterStaleHandle = 9,
    /** @brief A native call failed. window_state_adapter_last_error holds its error. */
    WindowStateAdapterNativeFailure = 10,
    /** @brief The action kind is a host-side kind, or is unknown. Nothing was executed for it. */
    WindowStateAdapterInvalidAction = 11,
    /** @brief A SetGeometry extent is zero or above WaddleWindowStateAdapterMaxExtent. */
    WindowStateAdapterInvalidGeometry = 12,
    /** @brief EnterFullscreen while a saved fullscreen state already exists. */
    WindowStateAdapterAlreadyFullscreen = 13,
    /** @brief LeaveFullscreen without a saved fullscreen state. Nothing is invented. */
    WindowStateAdapterNoSavedState = 14,
    /** @brief destroy or release while a saved fullscreen state exists. */
    WindowStateAdapterBusy = 15
} window_state_adapter_status_t;

/** @brief Window rectangle in screen coordinates, mirroring RECT field order. */
typedef struct window_state_rect_t {
    /** @brief Left edge. */
    int32_t left;
    /** @brief Top edge. */
    int32_t top;
    /** @brief Right edge, exclusive as in RECT. */
    int32_t right;
    /** @brief Bottom edge, exclusive as in RECT. */
    int32_t bottom;
} window_state_rect_t;

/** @brief Window placement, mirroring the WINDOWPLACEMENT fields the adapter saves and restores. */
typedef struct window_state_placement_t {
    /** @brief WPF_* flags. */
    uint32_t flags;
    /** @brief SW_* show command (normal, minimized or maximized). */
    uint32_t show_command;
    /** @brief Minimized position, x. */
    int32_t min_x;
    /** @brief Minimized position, y. */
    int32_t min_y;
    /** @brief Maximized position, x. */
    int32_t max_x;
    /** @brief Maximized position, y. */
    int32_t max_y;
    /** @brief Normal (restored) rectangle. */
    window_state_rect_t normal;
} window_state_placement_t;

/**
 * @brief Table of every native operation the adapter needs.
 *
 * Every entry is required (non-NULL). Each entry takes context as its first argument, which is
 * passed unchanged from window_state_adapter_create. Entries return 1 on success and 0 on failure.
 * A failing entry sets the native error that last_error returns. Every entry is called only on the
 * owner thread, except current_thread_id, which any thread may call to identify itself.
 */
typedef struct window_state_native_api_t {
    /** @brief IsWindow. Returns 1 when the window exists. */
    int (*is_window)(void *context, uintptr_t window);
    /**
     * @brief GetWindowThreadProcessId.
     * @param[out] process_id Non-NULL; receives the owning process id on success.
     * @param[out] thread_id Non-NULL; receives the owning thread id on success. 0 is a failure.
     */
    int (*window_thread)(void *context, uintptr_t window, uint32_t *process_id, uint32_t *thread_id);
    /** @brief IsIconic. @param[out] iconic Non-NULL; 1 when the window is minimized. */
    int (*is_iconic)(void *context, uintptr_t window, int *iconic);
    /** @brief IsZoomed. @param[out] zoomed Non-NULL; 1 when the window is maximized. */
    int (*is_zoomed)(void *context, uintptr_t window, int *zoomed);
    /** @brief GetWindowLongPtrW(GWL_STYLE). @param[out] style Non-NULL; the 32-bit style, zero-extended. */
    int (*get_style)(void *context, uintptr_t window, int64_t *style);
    /** @brief SetWindowLongPtrW(GWL_STYLE, style). */
    int (*set_style)(void *context, uintptr_t window, int64_t style);
    /**
     * @brief ShowWindow, synchronous. The command is SW_MINIMIZE, SW_RESTORE or SW_MAXIMIZE.
     */
    int (*show_window)(void *context, uintptr_t window, int32_t command);
    /** @brief SetWindowPos. The flags are passed through unchanged. */
    int (*set_window_pos)(void *context, uintptr_t window, int32_t x, int32_t y, int32_t width,
                          int32_t height, uint32_t flags);
    /** @brief GetWindowPlacement. @param[out] placement Non-NULL. */
    int (*get_placement)(void *context, uintptr_t window, window_state_placement_t *placement);
    /** @brief SetWindowPlacement. @param[in] placement Non-NULL. */
    int (*set_placement)(void *context, uintptr_t window, const window_state_placement_t *placement);
    /** @brief GetWindowRect. @param[out] rect Non-NULL. */
    int (*get_rect)(void *context, uintptr_t window, window_state_rect_t *rect);
    /** @brief rcMonitor of MonitorFromWindow(MONITOR_DEFAULTTONEAREST). @param[out] rect Non-NULL. */
    int (*monitor_rect)(void *context, uintptr_t window, window_state_rect_t *rect);
    /** @brief GetForegroundWindow. Returns 0 when there is none. Not a failure by itself. */
    uintptr_t (*foreground_window)(void *context);
    /** @brief SetForegroundWindow. */
    int (*set_foreground_window)(void *context, uintptr_t window);
    /** @brief SetActiveWindow. Used only by the activation fallback. */
    int (*set_active_window)(void *context, uintptr_t window);
    /** @brief AttachThreadInput(from, to, attach). @p attach is 1 to attach, 0 to detach. */
    int (*attach_thread_input)(void *context, uint32_t from_thread, uint32_t to_thread, int attach);
    /** @brief PostMessageW. The adapter uses it only with WM_CLOSE. */
    int (*post_message)(void *context, uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam);
    /** @brief GetCurrentThreadId. Read at create only, to name the owner thread. */
    uint32_t (*current_thread_id)(void *context);
    /** @brief GetLastError. Read after a failed entry. */
    uint32_t (*last_error)(void *context);
} window_state_native_api_t;

/**
 * @brief Opaque adapter. Allocated by window_state_adapter_create and freed by
 *        window_state_adapter_destroy; the layout is private to the C file.
 */
typedef struct window_state_adapter_t window_state_adapter_t;

/**
 * @brief Create an adapter bound to the calling thread.
 *
 * @param[in] api Non-NULL table. Copied by value; every entry must be non-NULL.
 * @param[in] context Passed to every entry. Must stay valid until destroy. May be NULL.
 * @param[out] out_adapter Non-NULL slot. Set to NULL first, then to the new adapter on Ok.
 * @return Ok, InvalidArgument, or OutOfMemory. The owner thread is the one that made this call.
 */
window_state_adapter_status_t window_state_adapter_create(const window_state_native_api_t *api, void *context,
                                                          window_state_adapter_t **out_adapter);

/**
 * @brief Free an adapter and set the caller's slot to NULL.
 *
 * @param[in,out] adapter_slot Non-NULL slot holding an adapter. A NULL *adapter_slot is a successful no-op.
 * @return Ok; Busy when any entry has a saved fullscreen state (nothing is freed); InvalidArgument when
 *         the slot is NULL; WrongThread or Reentrant under the threading rules.
 */
window_state_adapter_status_t window_state_adapter_destroy(window_state_adapter_t **adapter_slot);

/**
 * @brief Bind a (window_id, generation) pair to an existing HWND and capture its identity.
 *
 * @param[in,out] adapter Non-NULL owner-thread adapter.
 * @param[in] window HWND value. Must exist and have a thread id other than 0.
 * @param[in] window_id Protocol window identity, nonzero.
 * @param[in] generation Protocol generation, nonzero.
 * @return Ok, InvalidArgument (zero identity or NULL adapter), AlreadyAdmitted, CapacityExhausted,
 *         NotFound, NativeFailure, WrongThread, or Reentrant. Nothing is changed on any failure.
 */
window_state_adapter_status_t window_state_adapter_admit(window_state_adapter_t *adapter, uintptr_t window,
                                                         uint64_t window_id, uint32_t generation);

/**
 * @brief Remove an admitted entry. Makes no native call.
 *
 * @param[in,out] adapter Non-NULL owner-thread adapter.
 * @param[in] window_id Admitted identity.
 * @param[in] generation Admitted identity.
 * @return Ok, NotAdmitted, Busy (the entry has a saved fullscreen state), InvalidArgument, WrongThread,
 *         or Reentrant.
 */
window_state_adapter_status_t window_state_adapter_release(window_state_adapter_t *adapter, uint64_t window_id,
                                                           uint32_t generation);

/**
 * @brief Read the guest observation of an admitted window.
 *
 * @param[in,out] adapter Non-NULL owner-thread adapter.
 * @param[in] window Must equal the admitted HWND; any other value is InvalidArgument.
 * @param[in] window_id Admitted identity.
 * @param[in] generation Admitted identity.
 * @param[out] out_guest_state Non-NULL. Written only on Ok. Contains window_state.h state and capability bits.
 * @return Ok, InvalidArgument, NotAdmitted, StaleHandle (the entry is removed), NativeFailure
 *         (out is untouched), WrongThread, or Reentrant.
 */
window_state_adapter_status_t window_state_adapter_observe(window_state_adapter_t *adapter, uintptr_t window,
                                                           uint64_t window_id, uint32_t generation,
                                                           uint32_t *out_guest_state);

/**
 * @brief Execute a sequence of guest actions in order on an admitted window.
 *
 * Execution stops at the first action that does not complete. *out_completed is the number of
 * actions that completed before it, so the value can be passed to waddle_window_state_complete.
 *
 * @param[in,out] adapter Non-NULL owner-thread adapter.
 * @param[in] window Must equal the admitted HWND; any other value is InvalidArgument.
 * @param[in] window_id Admitted identity.
 * @param[in] generation Admitted identity.
 * @param[in] actions Non-NULL when count is nonzero; read by value during the call.
 * @param[in] count Number of actions, at most WaddleWindowStateMaxActions.
 * @param[out] out_completed Non-NULL. Written on every status after argument validation.
 * @return Ok, InvalidArgument, NotAdmitted, StaleHandle (the entry is removed), NativeFailure,
 *         InvalidAction, InvalidGeometry, AlreadyFullscreen, NoSavedState, WrongThread, or Reentrant.
 *         The status describes the first action that did not complete.
 */
window_state_adapter_status_t window_state_adapter_execute(window_state_adapter_t *adapter, uintptr_t window,
                                                           uint64_t window_id, uint32_t generation,
                                                           const waddle_window_state_action_t *actions,
                                                           uint64_t count, uint64_t *out_completed);

/**
 * @brief Native error of the most recent failed native call, or 0.
 *
 * @param[in] adapter Non-NULL adapter. Owner thread only; otherwise returns 0.
 * @return The value of GetLastError at the failure, or 0 when no native call has failed.
 */
uint32_t window_state_adapter_last_error(const window_state_adapter_t *adapter);

/**
 * @brief The Win32 native table of this module.
 *
 * @return A pointer to a static table under _WIN32; NULL on every other platform.
 * @note Thread safety: the table is immutable and may be read from any thread.
 */
const window_state_native_api_t *window_state_native_api(void);

#endif
