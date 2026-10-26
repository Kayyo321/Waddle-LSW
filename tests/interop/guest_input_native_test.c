/**
 * @file guest_input_native_test.c
 * @brief Windows smoke test for the guest keyboard executor, run on an interactive Windows desktop.
 *
 * Creates a visible window in this process, makes it foreground and drives the executor's real
 * SendInput path (guest_input_win32_native()). Checks the window messages it receives, toggles
 * Num Lock through the lock planner and restores it, and reloads the current keyboard layout
 * (which must not change it). Exit code 0 when every check passes; otherwise nonzero, with one
 * line per failed check on stderr.
 *
 * This program is cross-built on Linux and executed only on a Windows CI runner with an
 * interactive desktop. Foreground activation can be refused by Windows; the program reports that
 * as a failed check rather than skipping it.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest_input.h"

enum {
    MessageCapacity = 1024,
    ScanA = 0x1E,
    ScanB = 0x30,
    ScanLeftShift = 0x2A,
    ScanF13 = 0x64,
    PumpMs = 300,
    LockPollMs = 1500,
    PairGeneration = 1,
};

/** @brief One window message the smoke window recorded. */
typedef struct message_record_t {
    UINT message;
    WPARAM wparam;
    unsigned scan;
} message_record_t;

static message_record_t g_messages[MessageCapacity];
static size_t g_message_count;
static int g_failures;

/** @brief Records one failed check. */
static void check_failed(const char *what, int line) {
    fprintf(stderr, "FAIL line %d: %s\n", line, what);
    g_failures++;
}

#define CHECK_TRUE(condition, what) \
    do { if (!(condition)) check_failed(what, __LINE__); } while (0)

/** @brief Window procedure: records WM_KEYDOWN, WM_KEYUP and WM_CHAR. */
static LRESULT CALLBACK smoke_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR) {
        if (g_message_count < MessageCapacity) {
            g_messages[g_message_count].message = message;
            g_messages[g_message_count].wparam = wparam;
            g_messages[g_message_count].scan = (unsigned)((lparam >> 16) & 0xFF);
            g_message_count++;
        }
    }
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(window, message, wparam, lparam);
}

/** @brief Dispatches queued messages for at least `milliseconds`. */
static void pump(DWORD milliseconds) {
    DWORD start = GetTickCount();
    MSG message;
    while (GetTickCount() - start < milliseconds) {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
}

/** @brief Index of the first recorded message matching kind, scan and wparam, or SIZE_MAX. */
static size_t find_message(UINT message, unsigned scan, WPARAM wparam, int match_wparam) {
    size_t index;
    for (index = 0; index < g_message_count; index++) {
        if (g_messages[index].message != message || g_messages[index].scan != scan) continue;
        if (match_wparam && g_messages[index].wparam != wparam) continue;
        return index;
    }
    return (size_t)-1;
}

/** @brief Lock mask of the current toggle states, in the protocol bit order. */
static uint8_t current_lock_mask(void) {
    uint8_t mask = 0;
    if (GetKeyState(VK_CAPITAL) & 1) mask |= WaddleKeyboardLockCaps;
    if (GetKeyState(VK_NUMLOCK) & 1) mask |= WaddleKeyboardLockNum;
    if (GetKeyState(VK_SCROLL) & 1) mask |= WaddleKeyboardLockScroll;
    return mask;
}

/** @brief Reads the current keyboard layout KLID into eight ASCII bytes. Returns 0 on success. */
static int read_klid(uint8_t out[GuestInputKlidBytes]) {
    wchar_t name[KL_NAMELENGTH];
    size_t index;
    if (!GetKeyboardLayoutNameW(name)) return -1;
    for (index = 0; index < GuestInputKlidBytes; index++) {
        if (name[index] > 0x7F) return -1;
        out[index] = (uint8_t)name[index];
    }
    return 0;
}

/** @brief Waits until Num Lock reads `wanted`, pumping messages. Returns 1 when it does. */
static int wait_num_lock(int wanted) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < LockPollMs) {
        if (((GetKeyState(VK_NUMLOCK) & 1) != 0) == (wanted != 0)) return 1;
        pump(20);
    }
    return ((GetKeyState(VK_NUMLOCK) & 1) != 0) == (wanted != 0);
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class;
    HWND window;
    const guest_input_win32_t *win32 = guest_input_win32_native();
    guest_input_t state;
    uint64_t lease = 0;
    uint8_t klid_before[GuestInputKlidBytes];
    uint8_t klid_after[GuestInputKlidBytes];
    uint8_t original_locks;
    uint8_t flipped_locks;
    uint64_t window_id;
    uint32_t serial = 0;
    size_t a_down;
    size_t a_up;
    size_t shift_down;
    size_t b_down;
    size_t b_up;
    size_t shift_up;
    size_t f13_down;
    size_t char_a;
    size_t char_b_upper;
    int foreground = 0;
    int layout_is_us = 0;

    memset(&window_class, 0, sizeof window_class);
    window_class.lpfnWndProc = smoke_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"WaddleGuestInputSmoke";
    CHECK_TRUE(RegisterClassW(&window_class) != 0, "RegisterClassW failed");
    window = CreateWindowExW(0, L"WaddleGuestInputSmoke", L"Waddle guest input smoke", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, instance, NULL);
    CHECK_TRUE(window != NULL, "CreateWindowExW failed");
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    pump(PumpMs);

    CHECK_TRUE(SetForegroundWindow(window) != 0, "SetForegroundWindow returned FALSE");
    for (int attempt = 0; attempt < 10 && !foreground; attempt++) {
        pump(100);
        foreground = GetForegroundWindow() == window;
    }
    CHECK_TRUE(foreground, "smoke window is not foreground; run on an interactive desktop");
    if (!foreground) {
        DestroyWindow(window);
        fprintf(stderr, "guest input native smoke: %d check(s) failed\n", g_failures);
        return 1;
    }

    CHECK_TRUE(guest_input_init(&state, win32) == GuestInputOk, "guest_input_init failed");
    CHECK_TRUE(guest_input_revoke(&state, &lease) == GuestInputOk && lease == 1, "initial revocation did not reserve epoch 1");
    window_id = (uint64_t)(uintptr_t)window;
    CHECK_TRUE(guest_input_focus(&state, window_id, PairGeneration, ++serial, lease, 1) == GuestInputOk,
        "InputFocus enter was not accepted");

    original_locks = current_lock_mask();
    CHECK_TRUE(guest_input_lock_state(&state, ++serial, original_locks, lease) == GuestInputOk,
        "InputLockState matching the current toggles was not accepted");

    CHECK_TRUE(read_klid(klid_before) == 0, "GetKeyboardLayoutNameW failed");
    layout_is_us = memcmp(klid_before, "00000409", GuestInputKlidBytes) == 0;

    /* 'a' down and up. */
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 30, 1) == GuestInputOk, "key a down rejected");
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 30, 0) == GuestInputOk, "key a up rejected");
    /* Shift down, b down and up, Shift up. */
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 42, 1) == GuestInputOk, "shift down rejected");
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 48, 1) == GuestInputOk, "key b down rejected");
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 48, 0) == GuestInputOk, "key b up rejected");
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 42, 0) == GuestInputOk, "shift up rejected");
    /* F13 down and up. */
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 183, 1) == GuestInputOk, "F13 down rejected");
    CHECK_TRUE(guest_input_key(&state, window_id, PairGeneration, ++serial, lease, 183, 0) == GuestInputOk, "F13 up rejected");
    pump(PumpMs);

    a_down = find_message(WM_KEYDOWN, ScanA, 0, 0);
    a_up = find_message(WM_KEYUP, ScanA, 0, 0);
    shift_down = find_message(WM_KEYDOWN, ScanLeftShift, 0, 0);
    b_down = find_message(WM_KEYDOWN, ScanB, 0, 0);
    b_up = find_message(WM_KEYUP, ScanB, 0, 0);
    shift_up = find_message(WM_KEYUP, ScanLeftShift, 0, 0);
    f13_down = find_message(WM_KEYDOWN, ScanF13, 0, 0);
    CHECK_TRUE(a_down != (size_t)-1, "no WM_KEYDOWN for scan 0x1E (a)");
    CHECK_TRUE(a_up != (size_t)-1 && a_up > a_down, "no ordered WM_KEYUP for scan 0x1E (a)");
    CHECK_TRUE(shift_down != (size_t)-1, "no WM_KEYDOWN for Left Shift");
    CHECK_TRUE(b_down != (size_t)-1 && b_down > shift_down, "b down not after Shift down");
    CHECK_TRUE(b_up != (size_t)-1 && b_up > b_down, "b up not after b down");
    CHECK_TRUE(shift_up != (size_t)-1 && shift_up > b_up, "Shift up not after b up");
    CHECK_TRUE(f13_down != (size_t)-1, "no WM_KEYDOWN for F13 (scan 0x64)");
    if (layout_is_us) {
        char_a = find_message(WM_CHAR, ScanA, 'a', 1);
        char_b_upper = find_message(WM_CHAR, ScanB, 'B', 1);
        CHECK_TRUE(char_a != (size_t)-1, "no WM_CHAR 'a' under layout 00000409");
        CHECK_TRUE(char_b_upper != (size_t)-1, "no WM_CHAR 'B' for Shift+b under layout 00000409");
    } else {
        fprintf(stderr, "note: layout %.8s is not 00000409; WM_CHAR checks skipped\n", (const char *)klid_before);
    }

    /* Num Lock: plan to the flipped state through the executor, confirm, then restore. */
    flipped_locks = (uint8_t)(original_locks ^ WaddleKeyboardLockNum);
    CHECK_TRUE(guest_input_lock_state(&state, ++serial, flipped_locks, lease) == GuestInputOk,
        "InputLockState flipping Num Lock was not accepted");
    CHECK_TRUE(wait_num_lock((original_locks & WaddleKeyboardLockNum) == 0), "Num Lock did not flip");
    CHECK_TRUE(guest_input_lock_state(&state, ++serial, original_locks, lease) == GuestInputOk,
        "InputLockState restoring Num Lock was not accepted");
    CHECK_TRUE(wait_num_lock((original_locks & WaddleKeyboardLockNum) != 0), "Num Lock was not restored");

    /* Layout: load the current KLID only. It must not change the layout. */
    CHECK_TRUE(guest_input_keyboard_layout(&state, klid_before, GuestInputKlidBytes) == GuestInputOk,
        "KeyboardLayout for the current KLID was not applied");
    pump(PumpMs);
    CHECK_TRUE(read_klid(klid_after) == 0 && memcmp(klid_before, klid_after, GuestInputKlidBytes) == 0,
        "current layout changed after loading the current KLID");

    CHECK_TRUE(guest_input_disconnect(&state) == GuestInputOk, "disconnect did not release cleanly");
    CHECK_TRUE(state.held_count == 0, "keys still held after disconnect");

    DestroyWindow(window);
    pump(PumpMs);
    if (g_failures == 0) {
        printf("guest input native smoke: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "guest input native smoke: %d check(s) failed\n", g_failures);
    return 1;
}
