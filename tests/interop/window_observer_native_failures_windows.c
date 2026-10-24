/**
 * @file window_observer_native_failures_windows.c
 * @brief Windows-only white-box fault tests for native HWND fact consistency.
 *
 * Synthetic API failures are injected only at the final PID/TID read and monitor-info read.
 * Every test uses real temporary HWNDs and invokes the exact production source. This fixture is
 * not a native smoke or installed-session acceptance test.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef enum identity_fault_t {
    IdentityFaultNone = 0,
    IdentityFaultChangedProcess,
    IdentityFaultChangedThread,
    IdentityFaultZeroFinal,
    IdentityFaultFailedFinal
} identity_fault_t;

static HWND test_identity_window;
static identity_fault_t test_identity_fault;
static unsigned test_identity_calls;
static int test_monitor_info_failure;

/** @brief Invoke the real Win32 identity query, optionally altering only its second result. */
static DWORD WINAPI test_get_window_thread_process_id(HWND window, LPDWORD process_id) {
    const DWORD thread_id = GetWindowThreadProcessId(window, process_id);
    if (window != test_identity_window || test_identity_fault == IdentityFaultNone) return thread_id;
    test_identity_calls += 1u;
    if (test_identity_calls < 2u) return thread_id;
    switch (test_identity_fault) {
    case IdentityFaultChangedProcess:
        *process_id += 1u;
        return thread_id;
    case IdentityFaultChangedThread:
        return thread_id + 1u;
    case IdentityFaultZeroFinal:
        *process_id = 0u;
        return thread_id;
    case IdentityFaultFailedFinal:
        *process_id = 0u;
        return 0u;
    case IdentityFaultNone:
        break;
    }
    return thread_id;
}

/** @brief Invoke the real monitor query unless the one-shot synthetic failure is armed. */
static BOOL WINAPI test_get_monitor_info_w(HMONITOR monitor, LPMONITORINFO info) {
    if (test_monitor_info_failure) {
        test_monitor_info_failure = 0;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return GetMonitorInfoW(monitor, info);
}

#define GetWindowThreadProcessId test_get_window_thread_process_id
#define GetMonitorInfoW test_get_monitor_info_w
#include "../../src/interop/window_observer_windows.c"
#undef GetWindowThreadProcessId
#undef GetMonitorInfoW

/** @brief Print a failed assertion with source line and Win32 status. */
#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native observer fault check failed at line %d: %s (win32=%lu)\n", \
                __LINE__, #condition, (unsigned long)GetLastError()); \
        goto cleanup; \
    } \
} while (0)

/** @brief Query a real temporary window after zeroing the caller snapshot. */
static window_observer_query_result_t test_query_facts(HWND window, interop_window_facts_t *facts) {
    memset(facts, 0, sizeof(*facts));
    return native_query_facts(NULL, hwnd_to_u64(window), facts);
}

/** @brief Exercise synthetic fact failures and real monitor-query behavior using real HWNDs. */
int main(void) {
    int result = 1;
    window_observer_t *observer = NULL;
    HWND identity_window = NULL;
    HWND popup_window = NULL;
    DPI_AWARENESS_CONTEXT previous_dpi = NULL;
    interop_window_facts_t facts;
    RECT monitor_rect;
    HMONITOR monitor;
    MONITORINFO monitor_info;
    previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(previous_dpi != NULL);
    const window_observer_config_t config = {
        .api = NULL,
        .api_context = NULL,
        .first_stamp = 1,
        .preedit_window = 0,
        .own_process_id = GetCurrentProcessId(),
        .uac_mode = InteropWindowUacWindow,
    };
    Check(window_observer_create(&config, &observer) == WindowObserverStatusOk);
    Check(window_observer_start(observer) == WindowObserverStatusOk);
    identity_window = CreateWindowExW(0, L"STATIC", L"identity fault target",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 30, 30, 360, 220,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    Check(identity_window != NULL);
    test_identity_window = identity_window;
    Check(test_query_facts(identity_window, &facts) == WindowObserverQueryExists);

    const identity_fault_t identity_faults[] = {
        IdentityFaultChangedProcess,
        IdentityFaultChangedThread,
        IdentityFaultZeroFinal,
        IdentityFaultFailedFinal,
    };
    for (size_t index = 0; index < sizeof(identity_faults) / sizeof(identity_faults[0]); ++index) {
        test_identity_calls = 0u;
        test_identity_fault = identity_faults[index];
        Check(test_query_facts(identity_window, &facts) == WindowObserverQueryFailed);
        Check(test_identity_calls >= 2u);
        test_identity_fault = IdentityFaultNone;
        Check(test_query_facts(identity_window, &facts) == WindowObserverQueryExists);
    }

    popup_window = CreateWindowExW(0, L"STATIC", L"",
        WS_POPUP | WS_VISIBLE, 50, 50, 240, 160,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    Check(popup_window != NULL);
    Check(test_query_facts(popup_window, &facts) == WindowObserverQueryExists);
    test_monitor_info_failure = 1;
    Check(test_query_facts(popup_window, &facts) == WindowObserverQueryFailed);
    Check(test_monitor_info_failure == 0);
    Check(test_query_facts(popup_window, &facts) == WindowObserverQueryExists);

    monitor = MonitorFromWindow(popup_window, MONITOR_DEFAULTTONULL);
    Check(monitor != NULL);
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = (DWORD)sizeof(monitor_info);
    Check(GetMonitorInfoW(monitor, &monitor_info) != 0);
    monitor_rect = monitor_info.rcMonitor;
    Check(native_is_fullscreen(popup_window, 0, WS_POPUP, &monitor_rect) == 1);
    RECT small_rect = {monitor_rect.left, monitor_rect.top,
                       monitor_rect.left + 1, monitor_rect.top + 1};
    Check(native_is_fullscreen(popup_window, 0, WS_POPUP, &small_rect) == 0);
    Check(native_is_fullscreen(popup_window, 0, WS_CAPTION, &small_rect) == 0);
    printf("native observer synthetic failures: final PID/TID guards, monitor-info failure, and real fullscreen queries passed\n");
    result = 0;
cleanup:
    test_identity_fault = IdentityFaultNone;
    test_monitor_info_failure = 0;
    test_identity_window = NULL;
    if (popup_window != NULL && !DestroyWindow(popup_window)) result = 1;
    if (identity_window != NULL && !DestroyWindow(identity_window)) result = 1;
    if (observer != NULL) {
        for (unsigned attempt = 0; observer != NULL && attempt < 3; ++attempt) {
            if (window_observer_destroy(&observer) == WindowObserverStatusOk) break;
            Sleep(1);
        }
        if (observer != NULL) result = 1;
    }
    if (previous_dpi != NULL && SetThreadDpiAwarenessContext(previous_dpi) == NULL) result = 1;
    return result;
}

#undef Check
