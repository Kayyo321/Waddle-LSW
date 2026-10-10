/** @file windows_lease.c
 * @brief Executable Win32 API doubles for the production V3 adapter.
 * Runs without a desktop on Linux; also strict-cross-links with the real Windows
 * SDK types. These deterministic observations do not qualify native SendInput.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "waddle/av_protocol.h"
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#else
/* Narrow Win32 ABI-shaped types/constants used by av_windows.c. No OS emulation. */
typedef void *HWND, *HANDLE, *HWINEVENTHOOK, *HDESK, *HMONITOR, *HMODULE;
typedef void *DPI_AWARENESS_CONTEXT;
typedef uint32_t DWORD, UINT;
typedef int32_t LONG, BOOL, HRESULT;
typedef uint16_t WCHAR;
typedef intptr_t LONG_PTR, LPARAM;
typedef uintptr_t WPARAM;
typedef uint64_t ULONGLONG;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { uint16_t wVk, wScan; DWORD dwFlags, time; uintptr_t dwExtraInfo; } KEYBDINPUT;
typedef struct { LONG dx, dy; DWORD mouseData, dwFlags, time; uintptr_t dwExtraInfo; } MOUSEINPUT;
typedef struct { DWORD type; union { KEYBDINPUT ki; MOUSEINPUT mi; }; } INPUT;
typedef void (*WINEVENTPROC)(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD);
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define FAILED(value) ((value) < 0)
#define GW_OWNER 4
#define GA_ROOT 2
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_EX_TOOLWINDOW 0x80
#define WS_EX_NOACTIVATE 0x08000000
#define WS_EX_APPWINDOW 0x40000
#define WS_OVERLAPPEDWINDOW 0x00cf0000
#define SWP_FRAMECHANGED 0x20
#define SWP_NOZORDER 4
#define SWP_NOACTIVATE 0x10
#define SWP_NOMOVE 2
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define WM_CLOSE 0x10
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#define MONITOR_DEFAULTTONEAREST 2
#define CP_UTF8 65001
#define WC_ERR_INVALID_CHARS 0x80
#define CHILDID_SELF 0
#define OBJID_WINDOW 0
#define EVENT_SYSTEM_FOREGROUND 3
#define EVENT_SYSTEM_MINIMIZESTART 0x16
#define EVENT_SYSTEM_MINIMIZEEND 0x17
#define EVENT_OBJECT_CREATE 0x8000
#define EVENT_OBJECT_DESTROY 0x8001
#define EVENT_OBJECT_SHOW 0x8002
#define EVENT_OBJECT_HIDE 0x8003
#define EVENT_OBJECT_LOCATIONCHANGE 0x800b
#define WINEVENT_OUTOFCONTEXT 0
#define SYNCHRONIZE 0x100000
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define WAIT_TIMEOUT 258
#define UOI_IO 6
#define INPUT_MOUSE 0
#define INPUT_KEYBOARD 1
#define KEYEVENTF_EXTENDEDKEY 1
#define KEYEVENTF_KEYUP 2
#define KEYEVENTF_SCANCODE 8
#define MOUSEEVENTF_MOVE 1
#define MOUSEEVENTF_LEFTDOWN 2
#define MOUSEEVENTF_LEFTUP 4
#define MOUSEEVENTF_RIGHTDOWN 8
#define MOUSEEVENTF_RIGHTUP 0x10
#define MOUSEEVENTF_MIDDLEDOWN 0x20
#define MOUSEEVENTF_MIDDLEUP 0x40
#define MOUSEEVENTF_WHEEL 0x800
#define MOUSEEVENTF_HWHEEL 0x1000
#define MOUSEEVENTF_ABSOLUTE 0x8000
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(intptr_t)-4)
#endif
#define WaddleWindowsApiDouble 1
#define WaddleAvWindowsH 1
/* av_windows.h's ABI is intentionally repeated only for the headerless fixture. */
typedef int (*av_window_notify_t)(const av_message_t *, void *);
int av_windows_stop(void);
int av_windows_input(const av_message_t *message);
static const DWORD FixturePid = 77;
static DWORD fixture_thread = 22;
static uint64_t fixture_now = 100;
static unsigned api_reads, insertions, successful_insertions, activation_calls, close_calls;
static unsigned hook_count, unhook_count, notification_count;
static int receiving_input, desktop_error, process_alive, set_foreground_fail;
static int fail_send, fail_release, send_action, activation_action, cursor_action;
static int fail_hook, fail_unhook, defer_creates, fail_publish, reenter_publish;
static unsigned foreground_reads_until_fault;
static HWND foreground, cursor_window;
static WINEVENTPROC native_callback;
static HWINEVENTHOOK hooks[3];
static av_message_t published[128];
static INPUT inserted[512];
static struct fixture_window_t {
    int live, visible, enabled, minimized;
    DWORD process;
    LONG_PTR style, extended;
    HWND owner;
} fixture_windows[6];
static HWND native_handle(unsigned id) { return (HWND)(uintptr_t)id; }
static unsigned native_index(HWND handle) { return (unsigned)(uintptr_t)handle; }
static struct fixture_window_t *fixture_window(HWND handle) {
    unsigned i = native_index(handle);
    return i > 0 && i < 6 ? &fixture_windows[i] : &fixture_windows[0];
}
static void foreground_event(HWND handle) {
    native_callback(hooks[2], EVENT_SYSTEM_FOREGROUND, handle, OBJID_WINDOW, CHILDID_SELF, 91, 1);
}
static DWORD fake_thread(void) { return fixture_thread; }
static ULONGLONG fake_now(void) { ++api_reads; return fixture_now; }
static HDESK fake_desktop(DWORD thread) { assert(thread == fixture_thread); ++api_reads; return desktop_error ? NULL : (HDESK)(uintptr_t)9; }
static BOOL fake_user_object(HANDLE object, int kind, void *value, DWORD size, DWORD *needed) {
    assert(object == (HANDLE)(uintptr_t)9 && kind == UOI_IO && size == sizeof(BOOL));
    *(BOOL *)value = receiving_input; *needed = sizeof(BOOL); ++api_reads; return !desktop_error;
}
static HANDLE fake_open(DWORD access, BOOL inherit, DWORD pid) {
    assert(access == (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION) && !inherit && pid == FixturePid);
    return (HANDLE)(uintptr_t)10;
}
static DWORD fake_process_id(HANDLE handle) { assert(handle == (HANDLE)(uintptr_t)10); ++api_reads; return FixturePid; }
static DWORD fake_wait(HANDLE handle, DWORD timeout) { assert(handle == (HANDLE)(uintptr_t)10 && !timeout); ++api_reads; return process_alive ? WAIT_TIMEOUT : 0; }
static BOOL fake_close(HANDLE handle) { assert(handle == (HANDLE)(uintptr_t)10); ++close_calls; return TRUE; }
static DWORD fake_window_process(HWND handle, DWORD *pid) { ++api_reads; *pid = fixture_window(handle)->process; return 91; }
static LONG_PTR fake_get_style(HWND handle, int index) { ++api_reads; return index == GWL_EXSTYLE ? fixture_window(handle)->extended : fixture_window(handle)->style; }
static LONG_PTR fake_set_style(HWND handle, int index, LONG_PTR value) { (void)index; LONG_PTR old = fixture_window(handle)->style; fixture_window(handle)->style = value; return old; }
static BOOL fake_live(HWND handle) { ++api_reads; return fixture_window(handle)->live; }
static BOOL fake_visible(HWND handle) { ++api_reads; return fixture_window(handle)->visible; }
static BOOL fake_enabled(HWND handle) { ++api_reads; return fixture_window(handle)->enabled; }
static BOOL fake_minimized(HWND handle) { ++api_reads; return fixture_window(handle)->minimized; }
static HWND fake_ancestor(HWND handle, UINT kind) { assert(kind == GA_ROOT); ++api_reads; return handle; }
static HWND fake_owner(HWND handle, UINT kind) { assert(kind == GW_OWNER); ++api_reads; return fixture_window(handle)->owner; }
static BOOL fake_rect(HWND handle, RECT *bounds) { (void)handle; *bounds = (RECT){0, 0, 640, 480}; return TRUE; }
static HRESULT fake_dwm(HWND handle, DWORD kind, void *bounds, DWORD size) { assert(kind == DWMWA_EXTENDED_FRAME_BOUNDS && size == sizeof(RECT)); fake_rect(handle, bounds); return 0; }
static UINT fake_dpi(HWND handle) { (void)handle; return 96; }
static HMONITOR fake_monitor(HWND handle, DWORD mode) { (void)handle; (void)mode; return (HMONITOR)(uintptr_t)1; }
static BOOL fake_monitor_info(HMONITOR handle, MONITORINFO *info) { (void)handle; info->rcMonitor = (RECT){0,0,1920,1080}; return TRUE; }
static int fake_title(HWND handle, WCHAR *title, int length) { (void)handle; (void)title; (void)length; return 0; }
static int fake_utf8(UINT cp, DWORD flags, const WCHAR *text, int count, char *output, int capacity, const char *replacement, BOOL *used) {
    (void)cp; (void)flags; (void)text; (void)count; (void)output; (void)capacity; (void)replacement; (void)used; return 0;
}
static BOOL fake_position(HWND handle, HWND after, int x, int y, int w, int h, UINT flags) { (void)handle; (void)after; (void)x; (void)y; (void)w; (void)h; (void)flags; return TRUE; }
static BOOL fake_post(HWND handle, UINT kind, WPARAM wparam, LPARAM lparam) { (void)handle; (void)kind; (void)wparam; (void)lparam; return TRUE; }
static BOOL fake_show(HWND handle, int mode) { fixture_window(handle)->minimized = mode == SW_MINIMIZE; return TRUE; }
static void fake_set_error(DWORD error) { (void)error; }
static DWORD fake_error(void) { return 0; }
static DPI_AWARENESS_CONTEXT fake_dpi_context(DPI_AWARENESS_CONTEXT context) { (void)context; return (DPI_AWARENESS_CONTEXT)(uintptr_t)8; }
static HWINEVENTHOOK fake_hook(DWORD first, DWORD last, HMODULE module, WINEVENTPROC callback, DWORD pid, DWORD thread, DWORD flags) {
    assert(!module && !thread && flags == WINEVENT_OUTOFCONTEXT);
    unsigned slot = first == EVENT_OBJECT_CREATE ? 0 : first == EVENT_SYSTEM_MINIMIZESTART ? 1 : 2;
    assert(slot != 2 || (first == EVENT_SYSTEM_FOREGROUND && last == first && !pid));
    assert(slot == 2 || pid == FixturePid);
    native_callback = callback; ++hook_count;
    hooks[slot] = fail_hook == (int)slot + 1 ? NULL : (HWINEVENTHOOK)(uintptr_t)(11 + slot);
    return hooks[slot];
}
static BOOL fake_unhook(HWINEVENTHOOK hook) { assert(hook); ++unhook_count; return !fail_unhook; }
static BOOL fake_enum(BOOL (CALLBACK *callback)(HWND, LPARAM), LPARAM context) {
    for (unsigned i = 1; i < 6; ++i)
        if (fixture_windows[i].live && !callback(native_handle(i), context)) return FALSE;
    return TRUE;
}
static HWND fake_foreground(void) {
    ++api_reads;
    if (foreground_reads_until_fault && !--foreground_reads_until_fault)
        native_callback((HWINEVENTHOOK)(uintptr_t)999,EVENT_SYSTEM_FOREGROUND,foreground,0,0,91,1);
    return foreground;
}
static BOOL fake_activate(HWND handle) {
    ++activation_calls;
    if (set_foreground_fail) return FALSE;
    if (activation_action == 1) foreground_event(native_handle(3));
    if (activation_action == 2) native_callback(hooks[0], EVENT_OBJECT_DESTROY, handle, 0, 0, 91, 1);
    foreground = handle; foreground_event(handle);
    if (activation_action == 3) { foreground_event(native_handle(1)); foreground_event(handle); }
    return TRUE;
}
static HWND fake_hit(POINT point) { (void)point; return cursor_window; }
static BOOL fake_cursor(POINT *point) {
    *point = (POINT){20,20};
    if (cursor_action) { cursor_action = 0; foreground = native_handle(2); foreground_event(foreground); }
    return TRUE;
}
static int fake_metrics(int kind) { return kind == SM_CXVIRTUALSCREEN ? 1920 : kind == SM_CYVIRTUALSCREEN ? 1080 : 0; }
static UINT fake_send(UINT count, INPUT *inputs, int size) {
    assert(count == 1 && size == sizeof(INPUT) && insertions < 512);
    INPUT input = *inputs; inserted[insertions++] = input;
    int release = input.type == INPUT_KEYBOARD ? !!(input.ki.dwFlags & KEYEVENTF_KEYUP) :
        !!(input.mi.dwFlags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP));
    if (fail_send || (fail_release && release)) return 0;
    ++successful_insertions;
    int action = send_action; send_action = 0;
    if (action == 1) { foreground = native_handle(2); foreground_event(foreground); }
    if (action == 2) { for (unsigned i = 0; i < 65; ++i) foreground_event(foreground); }
    if (action == 3) { av_message_t event = {0}; assert(av_windows_input(&event) == -1); }
    if (action == 4) { foreground = native_handle(2); }
    if (action == 5) { foreground = native_handle(2); native_callback(hooks[0], EVENT_OBJECT_DESTROY, native_handle(1), 0, 0, 91, 1); }
    return 1;
}
#define GetCurrentThreadId fake_thread
#define GetTickCount64 fake_now
#define GetThreadDesktop fake_desktop
#define GetUserObjectInformationW fake_user_object
#define OpenProcess fake_open
#define GetProcessId fake_process_id
#define WaitForSingleObject fake_wait
#define CloseHandle fake_close
#define GetWindowThreadProcessId fake_window_process
#define GetWindowLongPtrW fake_get_style
#define SetWindowLongPtrW fake_set_style
#define IsWindow fake_live
#define IsWindowVisible fake_visible
#define IsWindowEnabled fake_enabled
#define IsIconic fake_minimized
#define GetAncestor fake_ancestor
#define GetWindow fake_owner
#define GetWindowRect fake_rect
#define DwmGetWindowAttribute fake_dwm
#define GetDpiForWindow fake_dpi
#define MonitorFromWindow fake_monitor
#define GetMonitorInfoW fake_monitor_info
#define GetWindowTextW fake_title
#define WideCharToMultiByte fake_utf8
#define SetWindowPos fake_position
#define PostMessageW fake_post
#define ShowWindow fake_show
#define SetLastError fake_set_error
#define GetLastError fake_error
#define SetThreadDpiAwarenessContext fake_dpi_context
#define SetWinEventHook fake_hook
#define UnhookWinEvent fake_unhook
#define EnumWindows fake_enum
#define GetForegroundWindow fake_foreground
#define SetForegroundWindow fake_activate
#define WindowFromPoint fake_hit
#define GetCursorPos fake_cursor
#define GetSystemMetrics fake_metrics
#define SendInput fake_send
#include "../../src/av/av_windows.c"

static int fixture_notify(const av_message_t *message, void *context) {
    assert(!context && notification_count < 128);
    if (message->type == MsgWindowCreateV3 && defer_creates) return 1;
    published[notification_count++] = *message;
    if (message->type == MsgInputEpochRevoked) {
        assert(lease_state.input.window_id == 0);
        for (unsigned i = 1; i < AvInputKeys; ++i) assert(!lease_state.input.keys[i]);
        if (message->lease_generation == 1) {
            assert(notification_count >= 2 && published[0].type == MsgWindowCreateV3);
            assert(windows[0].geometry.sequence == published[0].sequence);
        }
    }
    if (reenter_publish && message->type == MsgInputEpochReady) foreground_event(native_handle(3));
    return fail_publish == (int)message->type ? -1 : 0;
}
static void fixture_reset(void) {
    assert(!notification && !teardown_failed);
    memset(fixture_windows, 0, sizeof(fixture_windows));
    for (unsigned i = 1; i < 6; ++i) fixture_windows[i] = (struct fixture_window_t){
        .live=1,.visible=1,.enabled=1,.process=FixturePid,.style=WS_OVERLAPPEDWINDOW};
    fixture_windows[3].extended = WS_EX_TOOLWINDOW;
    fixture_windows[4].extended = WS_EX_APPWINDOW; fixture_windows[4].owner = native_handle(1);
    fixture_windows[5].process = 99;
    receiving_input = process_alive = 1; desktop_error = 0;
    foreground = native_handle(3); cursor_window = native_handle(1);
    fixture_now = 100; fixture_thread = 22;
    api_reads = insertions = successful_insertions = activation_calls = close_calls = 0;
    foreground_reads_until_fault = 0;
    hook_count = unhook_count = notification_count = 0;
    fail_send = fail_release = send_action = activation_action = cursor_action = 0;
    fail_hook = fail_unhook = defer_creates = fail_publish = reenter_publish = set_foreground_fail = 0;
}
static av_message_t host_message(av_message_type_t type, unsigned id, uint32_t code, uint32_t flags) {
    int index = find_window(native_handle(id));
    return (av_message_t){.type=type,.window_id=id,.sequence=index < 0 ? 999 : windows[index].geometry.sequence,
        .width=code,.flags=flags,.buffer_index=lease_state.input.serial+1,.lease_generation=lease_state.epoch};
}
static void acknowledge(void) {
    av_message_t ack = {.type=MsgInputEpochAck,.buffer_index=lease_state.input.serial+1,
        .lease_generation=lease_state.epoch};
    assert(av_windows_input(&ack) == 0);
    assert(lease_state.phase == AvLeaseUnfocusedReady);
}
static void start_ready(void) {
    fixture_reset(); assert(av_windows_start_v3(FixturePid, fixture_notify, NULL) == 0);
    assert(hook_count == 3 && lease_state.phase == AvLeaseAwaitAck && !lease_state.ever_anchored);
    assert(!insertions && !activation_calls); acknowledge();
}
static void focus(unsigned id) {
    av_message_t event = host_message(MsgInputFocusV3,id,0,1);
    assert(av_windows_input(&event) == 0 && lease_state.phase == AvLeaseActive);
    assert(lease_state.ever_anchored && lease_state.anchor_id == id);
}
static void key(unsigned code, unsigned down) {
    av_message_t event = host_message(MsgInputKeyV3,(unsigned)lease_state.input.window_id,code,down);
    assert(av_windows_input(&event) == 0);
}
static void finish(void) { assert(av_windows_stop() == 0 && close_calls == 1); }
static void startup_tests(void) {
    start_ready();
    foreground_event(NULL); foreground_event(native_handle(3)); foreground_event(native_handle(5));
    assert(av_windows_tick() == 0 && !lease_state.ever_anchored && !insertions);
    av_message_t stale = host_message(MsgInputFocusV3,99,0,1);
    assert(av_windows_input(&stale) == 0 && !activation_calls && !lease_state.ever_anchored);
    activation_action = 1; focus(1); assert(!insertions && activation_calls == 1);
    av_message_t off = host_message(MsgInputFocusV3,1,0,0);
    assert(av_windows_input(&off) == 0 && lease_state.ever_anchored && lease_state.anchor_id == 1);
    foreground_event(native_handle(3)); /* Event time 1 is deliberately very old. */
    assert(av_windows_tick() == -1 && lease_state.phase == AvLeaseGuestTerminal); finish();
    fixture_reset(); defer_creates=1;
    assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==0 && lease_state.phase==AvLeaseInitial);
    fixture_now += 100000; assert(av_windows_tick()==0 && av_windows_timeout(9000)==9000);
    defer_creates=0; assert(av_windows_refresh()==0 && lease_state.epoch==1); finish();
    fixture_reset(); fail_publish=MsgInputEpochRevoked;
    assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==-1 && notification_count==2 && !insertions);
    assert(close_calls==1);
    start_ready(); set_foreground_fail=1; stale=host_message(MsgInputFocusV3,1,0,1);
    assert(av_windows_input(&stale)==-1 && !lease_state.ever_anchored && !insertions); finish();
}
static void transition_tests(void) {
    start_ready(); focus(1); key(30,1);
    assert(lease_state.input.keys[30] && insertions==1);
    foreground=native_handle(2); foreground_event(foreground);
    assert(av_windows_tick()==0 && lease_state.epoch==2 && lease_state.phase==AvLeaseAwaitAck);
    assert(!lease_state.input.keys[30] && insertions==2 && (inserted[1].ki.dwFlags&KEYEVENTF_KEYUP));
    foreground_event(native_handle(2));
    av_message_t stale=host_message(MsgInputKeyV3,1,30,1); stale.lease_generation=1;
    unsigned reads=api_reads, count=observation_count;
    assert(av_windows_input(&stale)==0 && api_reads==reads && observation_count==count && insertions==2);
    acknowledge(); focus(2); key(30,1);
    foreground=native_handle(1); foreground_event(foreground);
    assert(av_windows_tick()==0 && lease_state.epoch==3);
    foreground=native_handle(2); foreground_event(foreground);
    assert(av_windows_tick()==-1 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); key(30,1); foreground=native_handle(2);
    assert(av_windows_tick()==0 && lease_state.epoch==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); key(30,1);
    foreground=native_handle(2); foreground_event(foreground); foreground=native_handle(1); foreground_event(foreground);
    assert(av_windows_tick()==-1 && !lease_state.input.keys[30]); finish();
}
static void accounting_tests(void) {
    start_ready(); focus(1); send_action=1; key(30,1);
    assert(lease_state.phase==AvLeaseAwaitAck && insertions==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); send_action=4; key(30,1);
    assert(lease_state.phase==AvLeaseAwaitAck && insertions==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); key(30,1); send_action=1; key(30,0);
    assert(lease_state.phase==AvLeaseAwaitAck && insertions==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); send_action=2;
    av_message_t event=host_message(MsgInputKeyV3,1,30,1);
    assert(av_windows_input(&event)==-1 && insertions==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); send_action=3; event=host_message(MsgInputKeyV3,1,30,1);
    assert(av_windows_input(&event)==-1 && insertions==2 && !lease_state.input.keys[30]); finish();
    start_ready(); focus(1); key(30,1); fail_release=1;
    foreground=native_handle(3); foreground_event(foreground);
    assert(av_windows_tick()==-1 && lease_state.input.keys[30]);
    assert(av_windows_stop()==-1 && lease_state.input.keys[30]);
    fail_release=0; assert(av_windows_stop()==0 && !lease_state.input.keys[30]);
    start_ready(); focus(1); event=host_message(MsgInputWheelV3,1,0,0); event.x=120; event.y=120; send_action=1;
    assert(av_windows_input(&event)==-1 && insertions==1 && inserted[0].mi.dwFlags==MOUSEEVENTF_HWHEEL); finish();
    start_ready(); focus(1); event=host_message(MsgInputButtonV3,1,1,1); cursor_action=1;
    assert(av_windows_input(&event)==0 && !insertions && lease_state.phase==AvLeaseAwaitAck); finish();
    start_ready(); focus(1); send_action=5; key(30,1);
    assert(lease_state.phase==AvLeaseAwaitAck && find_window(native_handle(1))<0 && insertions==2); finish();
}
static void eligibility_tests(void) {
    for (unsigned kind=0; kind<7; ++kind) {
        start_ready(); focus(1); key(30,1);
        if (kind==0) foreground=NULL;
        if (kind==1) foreground=native_handle(3);
        if (kind==2) foreground=native_handle(4); /* Capturable owned APPWINDOW is not recoverable. */
        if (kind==3) foreground=native_handle(5);
        if (kind==4) fixture_windows[1].enabled=0;
        if (kind==5) fixture_windows[1].minimized=1;
        if (kind==6) receiving_input=0;
        assert(av_windows_tick()==-1 && !lease_state.input.keys[30]); finish();
    }
    start_ready(); av_message_t event=host_message(MsgInputFocusV3,4,0,1);
    assert(av_windows_input(&event)==-1 && !activation_calls); finish();
    fixture_reset(); receiving_input=0;
    assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==-1 && !notification_count);
    start_ready(); process_alive=0; assert(av_windows_tick()==-1); finish();
}
static void legacy_tests(void) {
    fixture_reset();
    assert(av_windows_start(FixturePid,fixture_notify,NULL)==0 && hook_count==2);
    int index=find_window(native_handle(1)); assert(index>=0);
    av_message_t event={.type=MsgWindowClose,.window_id=1};
    assert(av_windows_apply(&event)==-1 && av_windows_status()==0);
    event=(av_message_t){.type=MsgWindowGeometry,.window_id=1,.sequence=windows[index].geometry.sequence,
        .width=640,.height=480,.dpi=96,.flags=AvWindowFullscreen};
    assert(av_windows_apply(&event)==0 && av_windows_status()==0);
    foreground=native_handle(1);
    event=(av_message_t){.type=MsgInputFocus,.window_id=1,.sequence=windows[index].geometry.sequence,
        .buffer_index=1,.flags=1};
    assert(av_windows_input(&event)==0);
    event.type=MsgInputKey; event.width=30; ++event.buffer_index;
    assert(av_windows_input(&event)==0 && input_state.keys[30]);
    foreground=native_handle(3);
    assert(av_windows_refresh()==-1 && !input_state.keys[30] && av_windows_status()==0);
    assert(av_windows_stop()==0 && close_calls==0);
}
static void failure_tests(void) {
    for (int hook=1; hook<=3; ++hook) {
        fixture_reset(); fail_hook=hook;
        assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==-1 && !notification_count && close_calls==1);
    }
    start_ready(); native_callback((HWINEVENTHOOK)(uintptr_t)999,EVENT_SYSTEM_FOREGROUND,NULL,0,0,91,1);
    assert(av_windows_status()==-1); finish();
    start_ready(); for (unsigned i=0;i<65;++i) foreground_event(NULL);
    assert(av_windows_status()==-1 && observation_count==64); finish();
    start_ready(); fixture_thread=23; foreground_event(NULL); fixture_thread=22;
    assert(av_windows_status()==-1); finish();
    start_ready(); fixture_now=99; assert(av_windows_tick()==-1); finish();
    fixture_reset(); assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==0);
    assert(av_windows_timeout(9000)==2000); fixture_now+=1999; assert(av_windows_timeout(9000)==1);
    fixture_now++; av_message_t event={.type=MsgInputEpochAck,.lease_generation=1,.buffer_index=1};
    assert(av_windows_input(&event)==-1 && published[notification_count-1].type!=MsgInputEpochReady); finish();
    fixture_reset(); assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==0); reenter_publish=1;
    event=(av_message_t){.type=MsgInputEpochAck,.lease_generation=1,.buffer_index=1};
    assert(av_windows_input(&event)==-1 && !lease_state.ever_anchored); finish();
    start_ready(); focus(1); activation_action=1; event=host_message(MsgInputFocusV3,2,0,1);
    assert(av_windows_input(&event)==-1 && !insertions); finish();
    start_ready(); focus(1); activation_action=3; event=host_message(MsgInputFocusV3,2,0,1);
    assert(av_windows_input(&event)==-1 && !insertions); finish();
    start_ready(); focus(1); foreground_reads_until_fault=2; event=host_message(MsgInputKeyV3,1,30,1);
    assert(av_windows_input(&event)==-1 && !insertions); finish();
    fixture_reset(); fixture_now=UINT64_MAX;
    assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==-1 && !insertions);
    start_ready(); fail_unhook=1; assert(av_windows_stop()==-1);
    assert(av_windows_start_v3(FixturePid,fixture_notify,NULL)==-1);
    fail_unhook=0; assert(av_windows_stop()==-1); /* Permanent failed-teardown latch. */
}
int main(void) {
    startup_tests(); transition_tests(); accounting_tests(); eligibility_tests(); legacy_tests(); failure_tests();
    puts("Win32 lease API doubles: startup, recovery, exact accounting, lifecycle, deadlines and teardown passed");
    return 0;
}
