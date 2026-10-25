/** @file window_observer_installed_windows.c
 * @brief Real foreign-process observer lifecycle acceptance on an interactive desktop.
 * @ownership Parent owns observer, graph, events and exact child handles. Child
 * owns its two HWNDs. No capture, pins, mappings or external buffers exist.
 */
#include "../../src/interop/window_observer_windows.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/** @brief Fixed independent test graph epoch, never used for a real mapping. */
static const uint64_t TestEpoch = 71;
/** @brief Own bounded child fixture class; no production message interception. */
static const wchar_t ChildClass[] = L"WaddleInteropInstalledObserverFixture";

typedef struct observed_events_t {
    uint64_t top;
    uint64_t dialog;
    uint64_t top_generation;
    uint64_t dialog_generation;
    int parent_ordered;
    int title_updated;
    int retired;
} observed_events_t;

static int names(DWORD parent_id, wchar_t *ready, wchar_t *stop,
                 wchar_t *top, wchar_t *dialog)
{
    return swprintf(ready, 128, L"Local\\WaddleInteropReady-%lu", (unsigned long)parent_id) > 0 &&
        swprintf(stop, 128, L"Local\\WaddleInteropStop-%lu", (unsigned long)parent_id) > 0 &&
        swprintf(top, 128, L"Waddle installed parent %lu", (unsigned long)parent_id) > 0 &&
        swprintf(dialog, 128, L"Waddle installed dialog %lu", (unsigned long)parent_id) > 0;
}

static int child_main(DWORD parent_id)
{
    wchar_t ready_name[128], stop_name[128], top_title[128], dialog_title[128];
    HANDLE ready = NULL, stop = NULL;
    HWND top = NULL, dialog = NULL;
    int result = 1;
    WNDCLASSW window_class = {0};
    if (!names(parent_id, ready_name, stop_name, top_title, dialog_title)) return 1;
    ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
    stop = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
    if (ready == NULL || stop == NULL) goto cleanup;
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = ChildClass;
    if (!RegisterClassW(&window_class)) goto cleanup;
    top = CreateWindowExW(0, ChildClass, top_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        70, 70, 420, 260, NULL, NULL, window_class.hInstance, NULL);
    if (top == NULL) goto cleanup;
    dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, ChildClass, dialog_title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 110, 250, 160,
        top, NULL, window_class.hInstance, NULL);
    if (dialog == NULL || !SetEvent(ready)) goto cleanup;
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        DWORD wait_result = MsgWaitForMultipleObjects(1, &stop, FALSE, 20, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) { result = 0; break; }
        if (wait_result == WAIT_FAILED) break;
        MSG message;
        for (unsigned count = 0; count < 256 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); ++count) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
cleanup:
    if (dialog != NULL && !DestroyWindow(dialog)) result = 1;
    if (top != NULL && !DestroyWindow(top)) result = 1;
    if (ready != NULL && !CloseHandle(ready)) result = 1;
    if (stop != NULL && !CloseHandle(stop)) result = 1;
    return result;
}

static int app_key(void *context, const interop_window_facts_t *facts,
                   uint8_t *key, uint32_t capacity, uint32_t *length)
{
    static const uint8_t Key[] = "a-installed-native";
    (void)context; (void)facts;
    if (key == NULL || length == NULL || capacity < sizeof(Key) - 1) return 1;
    memcpy(key, Key, sizeof(Key) - 1);
    *length = sizeof(Key) - 1;
    return 0;
}

static int drain_events(interop_window_graph_t *graph, observed_events_t *seen)
{
    interop_window_event_t event;
    for (unsigned count = 0; count < 512; ++count) {
        interop_window_graph_status_t result = interop_window_graph_next_event(graph, TestEpoch, &event);
        if (result == InteropWindowGraphStatusNone) return 1;
        if (result != InteropWindowGraphStatusOk) return 0;
        if (seen == NULL) continue;
        if (event.window == seen->top && event.kind == InteropWindowEventAnnounce)
            seen->top_generation = event.generation;
        if (event.window == seen->dialog && event.kind == InteropWindowEventAnnounce) {
            seen->dialog_generation = event.generation;
            seen->parent_ordered = seen->top_generation != 0 &&
                event.parent_window == seen->top &&
                event.parent_generation == seen->top_generation && event.role == InteropRoleDialog;
        }
        if (event.window == seen->dialog && event.kind == InteropWindowEventUpdate &&
            (event.changed_mask & InteropWindowChangeTitle) != 0 &&
            event.title_length == sizeof("Installed title change") - 1 &&
            memcmp(event.title, "Installed title change", sizeof("Installed title change") - 1) == 0)
            seen->title_updated = 1;
        if (event.window == seen->dialog && event.kind == InteropWindowEventRetireIntent)
            seen->retired = 1;
    }
    return 0;
}

static int pump(window_observer_t *observer, interop_window_graph_t *graph,
                observed_events_t *seen)
{
    MSG message;
    for (unsigned count = 0; count < 256; ++count) {
        if (window_observer_prepare_callbacks(observer, 0, 0) != WindowObserverStatusOk) return 0;
        if (!PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) break;
        if (window_observer_prepare_callbacks(observer, 0, 0) != WindowObserverStatusOk) return 0;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window_observer_status_t result = window_observer_refresh(observer, graph, TestEpoch, NULL);
    if (result != WindowObserverStatusOk && result != WindowObserverStatusMore) return 0;
    result = window_observer_pump(observer, graph, TestEpoch, 128, NULL);
    return (result == WindowObserverStatusOk || result == WindowObserverStatusMore) && drain_events(graph, seen);
}

static int complete_empty_retirements(window_observer_t *observer, interop_window_graph_t *graph)
{
    interop_window_retiring_entry_t entries[InteropMaxWindows];
    uint32_t count = 0;
    /* Factual fixture invariants: no capture or frame/pin API was ever started. */
    const interop_window_retire_proof_t proof = {1, 1};
    if (!drain_events(graph, NULL) || interop_window_graph_list_retiring(graph, entries,
            InteropMaxWindows, &count) != InteropWindowGraphStatusOk) return 0;
    for (uint32_t index = 0; index < count; ++index)
        if (interop_window_graph_complete_retire(graph, TestEpoch, entries[index].window,
                entries[index].generation, &proof) != InteropWindowGraphStatusOk) return 0;
    return window_observer_sync_tracked(observer) == WindowObserverStatusOk;
}

/** @brief Native fixture entry; borrowed CRT arguments, no ownership transfers.
 * @return Zero only on complete acceptance and cleanup, one on any failure.
 * @note One GUI thread per exact process; parent never shares observer/graph.
 */
int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--child") == 0) {
        wchar_t *end = NULL;
        unsigned long parent_id = wcstoul(argv[2], &end, 10);
        return parent_id != 0 && end != NULL && *end == 0 ? child_main((DWORD)parent_id) : 1;
    }
    if (argc != 1) return 1;
    HANDLE ready = NULL, stop = NULL;
    PROCESS_INFORMATION child = {0};
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    window_observer_t *observer = NULL;
    interop_window_graph_t *graph = NULL;
    wchar_t ready_name[128], stop_name[128], top_title[128], dialog_title[128];
    wchar_t executable[32768], command[32768 + 64];
    HWND top = NULL, dialog = NULL;
    observed_events_t seen = {0};
    interop_window_live_t live = {0};
    uint64_t first_generation = 0;
    int result = 1;
    int stop_owned = 0;
    DPI_AWARENESS_CONTEXT prior_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#define Check(condition) do { if (!(condition)) { fprintf(stderr, "installed observer line%d: %s win32=%lu\n", __LINE__, #condition, (unsigned long)GetLastError()); goto cleanup; } } while (0)
    Check(prior_dpi != NULL);
    Check(names(GetCurrentProcessId(), ready_name, stop_name, top_title, dialog_title));
    ready = CreateEventW(NULL, TRUE, FALSE, ready_name);
    Check(ready != NULL && GetLastError() != ERROR_ALREADY_EXISTS);
    stop = CreateEventW(NULL, TRUE, FALSE, stop_name);
    stop_owned = stop != NULL && GetLastError() != ERROR_ALREADY_EXISTS;
    Check(stop_owned);
    DWORD executable_length = GetModuleFileNameW(NULL, executable, 32768);
    Check(executable_length > 0 && executable_length < 32768);
    Check(swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" --child %lu",
        executable, (unsigned long)GetCurrentProcessId()) > 0);
    PROCESS_INFORMATION launched = {0};
    Check(CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &launched));
    child = launched;
    Check(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    top = FindWindowW(ChildClass, top_title);
    dialog = FindWindowW(ChildClass, dialog_title);
    DWORD top_process = 0, dialog_process = 0;
    Check(top != NULL && dialog != NULL);
    Check(GetWindowThreadProcessId(top, &top_process) != 0 && top_process == child.dwProcessId);
    Check(GetWindowThreadProcessId(dialog, &dialog_process) != 0 && dialog_process == child.dwProcessId);
    seen.top = (uint64_t)(uintptr_t)top; seen.dialog = (uint64_t)(uintptr_t)dialog;
    const interop_window_graph_config_t graph_config = {.session_epoch = TestEpoch,
        .uac_mode = InteropWindowUacWindow, .app_key_lookup = app_key};
    const window_observer_config_t observer_config = {.first_stamp = 1,
        .own_process_id = GetCurrentProcessId(), .uac_mode = InteropWindowUacWindow};
    Check(interop_window_graph_create(&graph_config, &graph) == InteropWindowGraphStatusOk);
    Check(window_observer_create(&observer_config, &observer) == WindowObserverStatusOk);
    Check(window_observer_bind_graph(observer, graph, TestEpoch) == WindowObserverStatusOk);
    Check(window_observer_start(observer) == WindowObserverStatusOk);
    ULONGLONG deadline = GetTickCount64() + 5000;
    do { Check(pump(observer, graph, &seen)); if (seen.parent_ordered) break; Sleep(5); } while (GetTickCount64() < deadline);
    Check(seen.parent_ordered);
    Check(interop_window_graph_find_live(graph, seen.dialog, &live) == InteropWindowGraphStatusOk);
    Check(live.process_id == child.dwProcessId && live.role == InteropRoleDialog);
    first_generation = live.generation;
    Check(window_observer_prepare_callbacks(observer, 0, 0) == WindowObserverStatusOk);
    DWORD_PTR message_result = 0;
    Check(SendMessageTimeoutW(dialog, WM_SETTEXT, 0, (LPARAM)L"Installed title change",
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &message_result) != 0 && message_result != 0);
    deadline = GetTickCount64() + 5000;
    do { Check(pump(observer, graph, &seen)); if (seen.title_updated) break; Sleep(5); } while (GetTickCount64() < deadline);
    Check(seen.title_updated);
    Check(window_observer_prepare_callbacks(observer, 0, 0) == WindowObserverStatusOk);
    Check(ShowWindowAsync(dialog, SW_HIDE));
    deadline = GetTickCount64() + 5000;
    do { Check(pump(observer, graph, &seen)); if (seen.retired) break; Sleep(5); } while (GetTickCount64() < deadline);
    Check(seen.retired);
    Check(interop_window_graph_check_action(graph, TestEpoch, seen.dialog, first_generation) == InteropWindowGraphStatusStale);
    Check(complete_empty_retirements(observer, graph));
    Check(window_observer_prepare_callbacks(observer, 0, 0) == WindowObserverStatusOk);
    Check(ShowWindowAsync(dialog, SW_SHOW));
    seen.dialog_generation = 0;
    deadline = GetTickCount64() + 5000;
    do { Check(pump(observer, graph, &seen)); if (seen.dialog_generation > first_generation) break; Sleep(5); } while (GetTickCount64() < deadline);
    Check(seen.dialog_generation > first_generation && seen.parent_ordered);
    Check(interop_window_graph_check_action(graph, TestEpoch, seen.dialog, first_generation) == InteropWindowGraphStatusStale);
    result = 0;
cleanup:
    if (observer != NULL) {
        if (window_observer_drain_protected_destroys(observer) != WindowObserverStatusOk) result = 1;
        if (window_observer_stop(observer) != WindowObserverStatusOk) result = 1;
        if (graph != NULL && !complete_empty_retirements(observer, graph)) result = 1;
        if (window_observer_destroy(&observer) != WindowObserverStatusOk) result = 1;
    }
    if (observer == NULL && graph != NULL && interop_window_graph_destroy(&graph) != InteropWindowGraphStatusOk) result = 1;
    if (stop_owned && !SetEvent(stop)) result = 1;
    if (child.hProcess != NULL) {
        if (WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
            result = 1;
            if (!TerminateProcess(child.hProcess, 1) || WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0)
                fprintf(stderr, "installed observer: exact child could not be reaped\n");
        } else {
            DWORD child_exit = 1;
            if (!GetExitCodeProcess(child.hProcess, &child_exit) || child_exit != 0) result = 1;
        }
        if (!CloseHandle(child.hProcess)) result = 1;
    }
    if (child.hThread != NULL && !CloseHandle(child.hThread)) result = 1;
    if (ready != NULL && !CloseHandle(ready)) result = 1;
    if (stop != NULL && !CloseHandle(stop)) result = 1;
    if (prior_dpi != NULL && SetThreadDpiAwarenessContext(prior_dpi) == NULL) result = 1;
    if (result == 0) puts("installed observer: real foreign graph admission, parent order, title, quarantine and generation passed");
    return result;
#undef Check
}
