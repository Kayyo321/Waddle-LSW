/**
 * @file window_observer_native_windows.c
 * @brief White-box real Win32 observer smoke with an authentic, empty graph binding.
 *
 * This fixture checks native facts, correlated WinEvent hints and protected Destroy delivery for
 * its own reserved candidate. It does not admit graph entries or claim retirement/backing proof.
 * The calling thread owns the graph, observer, HWND and DPI context; cleanup stops hooks before
 * freeing either owner and restores the original DPI context.
 */
#include "../../src/interop/window_observer_windows.c"
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/** @brief Epoch for the authentic native fixture graph. */
static const uint64_t NativeEpoch = UINT64_C(5);

/** @brief Supply a valid stable app key if a future fixture operation admits a window. */
static int native_app_key(void *context, const interop_window_facts_t *facts, uint8_t *key,
                          uint32_t capacity, uint32_t *length) {
    (void)context;
    (void)facts;
    static const uint8_t AppKey[] = "a-native-test";
    if (key == NULL || length == NULL || capacity < sizeof(AppKey) - 1u) return 1;
    memcpy(key, AppKey, sizeof(AppKey) - 1u);
    *length = (uint32_t)(sizeof(AppKey) - 1u);
    return 0;
}

/** @brief Establish the mandatory observer safe point before callback-capable owner-thread work. */
static int prepare_owner_work(window_observer_t *observer, uint64_t candidate_window) {
    const uint32_t may_admit = candidate_window != 0u ? 1u : 0u;
    return window_observer_prepare_callbacks(observer, candidate_window, may_admit) ==
           WindowObserverStatusOk;
}

/** @brief Dispatch only after a fresh callback reserve; returns zero if reservation fails. */
static int dispatch_messages(window_observer_t *observer, uint64_t candidate_window) {
    MSG message;
    for (unsigned count = 0; count < 256u; ++count) {
        if (!prepare_owner_work(observer, candidate_window)) return 0;
        if (!PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) break;
        if (!prepare_owner_work(observer, candidate_window)) return 0;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 1;
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
    interop_window_graph_t *graph = NULL;
    const DPI_AWARENESS_CONTEXT previous_dpi =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const window_observer_api_t *api = NULL;
    window_observer_stats_t before = {0};
    window_observer_stats_t after = {0};
    interop_window_facts_t facts;
    uint64_t window_key = 0;
    uint64_t deadline_ms = 0;
    uint32_t destroy_pending = 0u;
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
    const interop_window_graph_config_t graph_config = {
        .session_epoch = NativeEpoch,
        .uac_mode = InteropWindowUacWindow,
        .reserved0 = 0u,
        .app_key_lookup = native_app_key,
        .app_key_context = NULL,
        .now_ms = NULL,
        .now_ms_context = NULL,
    };
    Check(interop_window_graph_create(&graph_config, &graph) == InteropWindowGraphStatusOk);
    const window_observer_config_t config = {
        .api = NULL,
        .api_context = NULL,
        .first_stamp = 1,
        .preedit_window = 0,
        .own_process_id = GetCurrentProcessId(),
        .uac_mode = InteropWindowUacWindow,
    };
    Check(window_observer_create(&config, &observer) == WindowObserverStatusOk);
    Check(window_observer_bind_graph(observer, graph, NativeEpoch) == WindowObserverStatusOk);
    Check(window_observer_start(observer) == WindowObserverStatusOk);
    Check(window_observer_status(observer, &before) == WindowObserverStatusOk);

    Check(prepare_owner_work(observer, 0u));
    window = CreateWindowExW(0, L"STATIC", L"Waddle native observer smoke",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 240,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    Check(window != NULL);
    window_key = hwnd_to_u64(window);
    Check(window_observer_set_preedit_window(observer, window_key) == WindowObserverStatusOk);
    Check(prepare_owner_work(observer, window_key));
    memset(&facts, 0, sizeof(facts));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryExists);
    Check(facts.window == window_key && facts.root == window_key);
    Check(facts.owner == 0 && facts.owner_root == 0);
    Check(facts.process_id == GetCurrentProcessId());
    Check(facts.thread_id == GetCurrentThreadId());
    Check(facts.width > 0 && facts.height > 0 && facts.dpi >= 96);
    Check((facts.flags & InteropWindowFactVisible) != 0);
    Check(facts.title_length == 0); /* Native adapter never reads own-process titles. */
    /* Pure overlay/classification of actual native facts: preedit is the explicit own-window exception. */
    interop_window_facts_t overlaid = facts;
    interop_window_verdict_t verdict;
    overlay_facts(observer, &overlaid, 1u, api->tick_ms(NULL));
    Check((overlaid.flags & InteropWindowFactAgentOwned) != 0u);
    Check((overlaid.flags & InteropWindowFactAgentPreedit) != 0u);
    Check(interop_window_graph_classify(&overlaid, InteropWindowUacWindow, &verdict) ==
          InteropWindowGraphStatusOk);
    Check(verdict.kind == InteropWindowVerdictEligible && verdict.role == InteropRoleImePreedit);
    overlaid.flags &= ~(uint32_t)InteropWindowFactAgentPreedit;
    Check(interop_window_graph_classify(&overlaid, InteropWindowUacWindow, &verdict) ==
          InteropWindowGraphStatusOk);
    Check(verdict.kind == InteropWindowVerdictNotExported && verdict.reason == InteropWindowReasonExcludedAgent);

    deadline_ms = GetTickCount64() + 10000u;
    do {
        Check(dispatch_messages(observer, window_key));
        Check(window_observer_status(observer, &after) == WindowObserverStatusOk);
        if (has_target_create_show_hint(observer, window_key)) break;
        Sleep(1u);
    } while (GetTickCount64() < deadline_ms);
    Check(has_target_create_show_hint(observer, window_key));
    Check(after.hints_wrong_thread == 0u);

    Check(prepare_owner_work(observer, window_key));
    ShowWindow(window, SW_HIDE);
    memset(&facts, 0, sizeof(facts));
    Check(prepare_owner_work(observer, window_key));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryExists);
    Check((facts.flags & InteropWindowFactVisible) == 0u);

    Check(prepare_owner_work(observer, window_key));
    Check(DestroyWindow(window) != 0);
    window = NULL;
    memset(&facts, 0, sizeof(facts));
    Check(prepare_owner_work(observer, window_key));
    Check(api->query_facts(NULL, window_key, &facts) == WindowObserverQueryGone);
    deadline_ms = GetTickCount64() + 10000u;
    do {
        Check(dispatch_messages(observer, window_key));
        Check(window_observer_destroy_pending(observer, window_key, &destroy_pending) ==
              WindowObserverStatusOk);
        if (destroy_pending != 0u) break;
        Sleep(1u);
    } while (GetTickCount64() < deadline_ms);
    Check(destroy_pending == 1u);
    Check(window_observer_drain_protected_destroys(observer) == WindowObserverStatusOk);
    Check(window_observer_destroy_pending(observer, window_key, &destroy_pending) ==
          WindowObserverStatusOk && destroy_pending == 0u);
    Check(window_observer_set_preedit_window(observer, 0u) == WindowObserverStatusOk);
    Check(window_observer_stop(observer) == WindowObserverStatusOk);
    Check(window_observer_status(observer, &after) == WindowObserverStatusOk);
    Check(after.state == WindowObserverStateStopped);
    Check(window_observer_destroy(&observer) == WindowObserverStatusOk);
    Check(observer == NULL);
    Check(interop_window_graph_destroy(&graph) == InteropWindowGraphStatusOk);
    Check(graph == NULL);
    printf("native observer: real facts, target-correlated hints, reserved-candidate protected Destroy, and explicit teardown passed; hints=%" PRIu64 "\n",
        after.hints_received - before.hints_received);
    result = 0;
cleanup:
    if (observer != NULL) {
        if (window_observer_status(observer, &after) == WindowObserverStatusOk &&
            (after.state == WindowObserverStateStarted || after.state == WindowObserverStateFailed ||
             after.state == WindowObserverStateStopping)) {
            const window_observer_status_t stop_status = window_observer_stop(observer);
            if (stop_status != WindowObserverStatusOk) {
                fprintf(stderr, "native observer cleanup: stop failed (%u)\n", (unsigned)stop_status);
                result = 1;
            }
        }
    }
    if (window != NULL) {
        if (observer != NULL && !prepare_owner_work(observer, hwnd_to_u64(window))) result = 1;
        if (!DestroyWindow(window)) {
            fprintf(stderr, "native observer cleanup: DestroyWindow failed\n");
            result = 1;
        }
    }
    if (observer != NULL) {
        (void)window_observer_drain_protected_destroys(observer);
        if (window_observer_destroy(&observer) != WindowObserverStatusOk) {
            fprintf(stderr, "native observer cleanup: observer retained after safe destroy attempt\n");
            result = 1;
        }
    }
    if (observer == NULL && graph != NULL) {
        if (interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk) {
            fprintf(stderr, "native observer cleanup: graph retained because it is not empty\n");
            result = 1;
        }
    } else if (graph != NULL) {
        fprintf(stderr, "native observer cleanup: retained graph with observer's borrowed binding\n");
        result = 1;
    }
    if (previous_dpi != NULL && SetThreadDpiAwarenessContext(previous_dpi) == NULL) {
        fprintf(stderr, "native observer cleanup: failed DPI context restore\n");
        result = 1;
    }
    return result;
#undef Check
}
