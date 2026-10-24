/**
 * @file window_observer_native_windows.c
 * @brief White-box real Win32 adapter smoke; no graph operations, API double, driver or capture.
 *
 * The fixture includes the exact production observer C source so it can correlate a real hook hint
 * with its temporary HWND without adding a production test API. The calling thread owns one HWND
 * and one observer. Every exit attempts both lifecycles and restores DPI context. Failure is never a skip.
 */
#include "../../src/interop/window_observer_windows.c"
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/** @brief Drain queued messages without waiting; no borrowed message escapes. */
static void dispatch_messages(void) {
    MSG message;
    for (unsigned count = 0; count < 256; ++count) {
        if (!PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

/** @brief True when the production dirty table contains a create/show hint for this exact HWND. */
static int has_target_create_show_hint(const window_observer_t *observer, uint64_t window_key) {
    for (uint32_t index = 0; index < observer->dirty_count; ++index) {
        const dirty_entry_t *entry = &observer->dirty[index];
        if (entry->window == window_key && (entry->kinds & (DirtyKindCreate | DirtyKindShow)) != 0u) {
            return 1;
        }
    }
    return 0;
}

/** @brief Smoke owner; returns zero only after checks and complete cleanup. */
int main(void) {
    int result = 1;
    HWND window = NULL;
    window_observer_t *observer = NULL;
    const DPI_AWARENESS_CONTEXT previous_dpi =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const window_observer_api_t *api = NULL;
    window_observer_stats_t before = {0};
    window_observer_stats_t after = {0};
    interop_window_facts_t facts;
    uint64_t window_key = 0;
    uint64_t deadline_ms = 0;
#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native observer check failed at line %d: %s (win32=%lu)\n", \
                __LINE__, #condition, (unsigned long)GetLastError()); \
        goto cleanup; \
    } \
} while (0)
    Check(previous_dpi != NULL);
    api = window_observer_native_api();
    Check(api != NULL);
    Check(api->query_input_desktop(NULL) == WindowObserverDesktopDefault);
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
    Check(window_observer_status(observer, &before) == WindowObserverStatusOk);
    window = CreateWindowExW(0, L"STATIC", L"Waddle native observer smoke",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 240,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    Check(window != NULL);
    window_key = hwnd_to_u64(window);
    memset(&facts, 0, sizeof(facts));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryExists);
    Check(facts.window == window_key && facts.root == window_key);
    Check(facts.owner == 0 && facts.owner_root == 0);
    Check(facts.process_id == GetCurrentProcessId());
    Check(facts.thread_id == GetCurrentThreadId());
    Check(facts.width > 0 && facts.height > 0 && facts.dpi >= 96);
    Check((facts.flags & InteropWindowFactVisible) != 0);
    Check(facts.title_length == 0); /* Native adapter never reads own-process titles. */
    deadline_ms = GetTickCount64() + 10000;
    do {
        dispatch_messages();
        Check(window_observer_status(observer, &after) == WindowObserverStatusOk);
        if (has_target_create_show_hint(observer, window_key)) break;
        Sleep(1);
    } while (GetTickCount64() < deadline_ms);
    Check(has_target_create_show_hint(observer, window_key));
    Check(after.hints_wrong_thread == 0);
    ShowWindow(window, SW_HIDE);
    memset(&facts, 0, sizeof(facts));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryExists);
    Check((facts.flags & InteropWindowFactVisible) == 0);
    Check(DestroyWindow(window) != 0);
    window = NULL;
    memset(&facts, 0, sizeof(facts));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryGone);
    Check(window_observer_stop(observer) == WindowObserverStatusOk);
    Check(window_observer_status(observer, &after) == WindowObserverStatusOk);
    Check(after.state == WindowObserverStateStopped);
    Check(window_observer_destroy(&observer) == WindowObserverStatusOk);
    Check(observer == NULL);
    printf("native observer: real ownerless query, visibility, Gone, target-correlated hook and teardown passed; hints=%" PRIu64 "\n",
        after.hints_received - before.hints_received);
    result = 0;
cleanup:
    if (window != NULL && !DestroyWindow(window)) {
        fprintf(stderr, "native observer cleanup: DestroyWindow failed\n");
        result = 1;
    }
    if (observer != NULL) {
        /* Failed unhooks retain tokens; bounded retries never pretend cleanup succeeded. */
        for (unsigned attempt = 0; observer != NULL && attempt < 3; ++attempt) {
            if (window_observer_destroy(&observer) == WindowObserverStatusOk) break;
            dispatch_messages();
            Sleep(1);
        }
        if (observer != NULL) {
            fprintf(stderr, "native observer cleanup: retained hook owner after retries\n");
            result = 1;
        }
    }
    if (previous_dpi != NULL && SetThreadDpiAwarenessContext(previous_dpi) == NULL) {
        fprintf(stderr, "native observer cleanup: failed DPI context restore\n");
        result = 1;
    }
    return result;
#undef Check
}
