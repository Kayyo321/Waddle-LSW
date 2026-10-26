/**
 * @file guest_input_test.c
 * @brief Linux host tests for the guest keyboard executor (guest_input.c).
 *
 * A Win32 double records every SendInput record, scripts GetKeyState, LoadKeyboardLayoutW,
 * GetForegroundWindow and GetWindowThreadProcessId, and records PostMessageW. Each scenario
 * checks the exact injected INPUT sequence and the returned status. Equivalence with
 * av_input.c is checked over every evdev code. A seeded random sequence checks that the
 * injected stream never releases an up-key that is not down and ends with no key held.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "av_input.h"
#include "guest_input.h"

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

#define EXPECT_STATUS(expression, expected)                                              \
    do {                                                                                 \
        guest_input_status_t got_ = (expression);                                        \
        if (got_ != (expected)) {                                                        \
            fprintf(stderr, "%s:%d: %s returned %d, expected %d\n", __FILE__, __LINE__,  \
                #expression, (int)got_, (int)(expected));                                \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

enum {
    RecordCapacity = 1 << 16,
    PostCapacity = 256,
    WindowA = 0x1000,
    WindowB = 0x2000,
    GenerationA = 7,
    GenerationB = 9,
    ThreadId = 77,
    KeyStates = 256,
    StateIndexBits = 17,
};

/** @brief One INPUT record as the double saw it. */
typedef struct fake_record_t {
    DWORD type;
    WORD vk;
    WORD scan;
    DWORD flags;
} fake_record_t;

/** @brief One PostMessageW call as the double saw it. */
typedef struct fake_post_t {
    HWND window;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
} fake_post_t;

static fake_record_t g_records[RecordCapacity];
static size_t g_record_count;
static size_t g_send_calls;
static size_t g_fail_send_call;
static int g_fail_all_sends;
static SHORT g_key_state[KeyStates];
static HWND g_foreground;
static DWORD g_thread_id;
static fake_post_t g_posts[PostCapacity];
static size_t g_post_count;
static BOOL g_post_result;
static HKL g_load_result;
static int g_load_fail;
static wchar_t g_last_klid[16];
static size_t g_load_calls;
static UINT g_last_load_flags;
static char g_window_token;
static HWND g_window_handle = (HWND)&g_window_token;
static HKL g_layout_handle = (HKL)(uintptr_t)0x04090409u;

/** @brief Restores the double to its default script. Records and counters start empty. */
static void fake_reset(void) {
    memset(g_records, 0, sizeof g_records);
    g_record_count = 0;
    g_send_calls = 0;
    g_fail_send_call = 0;
    g_fail_all_sends = 0;
    memset(g_key_state, 0, sizeof g_key_state);
    g_foreground = g_window_handle;
    g_thread_id = ThreadId;
    g_post_count = 0;
    g_post_result = 1;
    g_load_result = g_layout_handle;
    g_load_fail = 0;
    memset(g_last_klid, 0, sizeof g_last_klid);
    g_load_calls = 0;
    g_last_load_flags = 0;
}

/** @brief Test double of SendInput: records every record, honors the failure script. */
static UINT fake_send_input(UINT count, LPINPUT inputs, int size) {
    UINT index;
    g_send_calls++;
    if (size != (int)sizeof(INPUT)) return 0;
    if (g_fail_all_sends || (g_fail_send_call != 0 && g_send_calls == g_fail_send_call)) return 0;
    for (index = 0; index < count; index++) {
        CHECK(g_record_count < RecordCapacity);
        g_records[g_record_count].type = inputs[index].type;
        g_records[g_record_count].vk = inputs[index].ki.wVk;
        g_records[g_record_count].scan = inputs[index].ki.wScan;
        g_records[g_record_count].flags = inputs[index].ki.dwFlags;
        g_record_count++;
    }
    return count;
}

/** @brief Test double of GetKeyState. */
static SHORT fake_get_key_state(int virtual_key) { return g_key_state[virtual_key & 0xFF]; }

/** @brief Test double of LoadKeyboardLayoutW: records the KLID and flags. */
static HKL fake_load_keyboard_layout(LPCWSTR klid, UINT flags) {
    size_t index;
    g_load_calls++;
    g_last_load_flags = flags;
    for (index = 0; index < 15 && klid[index] != L'\0'; index++) g_last_klid[index] = klid[index];
    g_last_klid[index] = L'\0';
    return g_load_fail ? NULL : g_load_result;
}

/** @brief Test double of PostMessageW: records the call and returns the scripted result. */
static BOOL fake_post_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    CHECK(g_post_count < PostCapacity);
    g_posts[g_post_count].window = window;
    g_posts[g_post_count].message = message;
    g_posts[g_post_count].wparam = wparam;
    g_posts[g_post_count].lparam = lparam;
    g_post_count++;
    return g_post_result;
}

/** @brief Test double of GetForegroundWindow. */
static HWND fake_get_foreground_window(void) { return g_foreground; }

/** @brief Test double of GetWindowThreadProcessId: the scripted thread for the foreground window only. */
static DWORD fake_get_window_thread_process_id(HWND window, LPDWORD process_id) {
    if (process_id) *process_id = 4321;
    return (window != NULL && window == g_foreground) ? g_thread_id : 0;
}

static const guest_input_win32_t FakeWin32 = {
    fake_send_input,
    fake_get_key_state,
    fake_load_keyboard_layout,
    fake_post_message,
    fake_get_foreground_window,
    fake_get_window_thread_process_id,
};

/** @brief Expected record of one injected INPUT. */
typedef struct expected_t {
    WORD vk;
    WORD scan;
    DWORD flags;
} expected_t;

/** @brief Checks the record count and then every record, in order, against `expected`. */
static void expect_records(const expected_t *expected, size_t count) {
    size_t index;
    CHECK(g_record_count == count);
    for (index = 0; index < count; index++) {
        CHECK(g_records[index].type == INPUT_KEYBOARD);
        CHECK(g_records[index].vk == expected[index].vk);
        CHECK(g_records[index].scan == expected[index].scan);
        CHECK(g_records[index].flags == expected[index].flags);
    }
}

/** @brief Clears the record log only. The double's script stays. */
static void records_clear(void) {
    g_record_count = 0;
}

/** @brief Starts a session: init, then the first revocation reserving epoch 1 (no held keys). */
static void session_start(guest_input_t *state) {
    uint64_t epoch = 0;
    EXPECT_STATUS(guest_input_init(state, &FakeWin32), GuestInputOk);
    EXPECT_STATUS(guest_input_revoke(state, &epoch), GuestInputOk);
    CHECK(epoch == 1);
    CHECK(state->epoch == 1);
}

/** @brief Serial helper: the next protocol serial for the current scenario. */
static uint32_t g_serial;
static uint32_t next_serial(void) { return ++g_serial; }

/** @brief Sets the scripted toggle states of the three lock keys. */
static void set_locks(int caps, int num, int scroll) {
    g_key_state[0x14] = (SHORT)(caps ? 1 : 0);
    g_key_state[0x90] = (SHORT)(num ? 1 : 0);
    g_key_state[0x91] = (SHORT)(scroll ? 1 : 0);
}

/** @brief Lock mask of the scripted toggle state, in the protocol bit order. */
static uint8_t lock_mask(int caps, int num, int scroll) {
    return (uint8_t)((caps ? 1 : 0) | (num ? 2 : 0) | (scroll ? 4 : 0));
}

/** @brief Focus helper for one pair in the current epoch with a fresh serial. */
static guest_input_status_t focus(guest_input_t *state, uint64_t window, uint64_t generation, uint8_t pressed) {
    return guest_input_focus(state, window, generation, next_serial(), state->epoch, pressed);
}

/** @brief Key helper for the current epoch with a fresh serial. */
static guest_input_status_t key(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t code, uint8_t pressed) {
    return guest_input_key(state, window, generation, next_serial(), state->epoch, code, pressed);
}

/** @brief Lock-state helper for the current epoch with a fresh serial. */
static guest_input_status_t locks(guest_input_t *state, uint8_t mask) {
    return guest_input_lock_state(state, next_serial(), mask, state->epoch);
}

/** @brief Scenario: argument checks and init rules. */
static void test_init_and_null_arguments(void) {
    guest_input_t state;
    guest_input_win32_t partial = FakeWin32;
    uint64_t epoch = 0;
    fake_reset();
    EXPECT_STATUS(guest_input_init(NULL, &FakeWin32), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_init(&state, NULL), GuestInputNullArgument);
    partial.send_input = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    partial = FakeWin32;
    partial.get_key_state = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    partial = FakeWin32;
    partial.load_keyboard_layout = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    partial = FakeWin32;
    partial.post_message = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    partial = FakeWin32;
    partial.get_foreground_window = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    partial = FakeWin32;
    partial.get_window_thread_process_id = NULL;
    EXPECT_STATUS(guest_input_init(&state, &partial), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_init(&state, &FakeWin32), GuestInputOk);
    CHECK(state.epoch == 0 && state.serial == 0 && !state.terminal && state.held_count == 0);
    EXPECT_STATUS(guest_input_revoke(NULL, &epoch), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_revoke(&state, NULL), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_admit_serial(NULL, 1, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_focus(NULL, WindowA, GenerationA, 1, 1, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_key(NULL, WindowA, GenerationA, 1, 1, 30, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_release(NULL, WindowA, GenerationA, 1, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_lock_state(NULL, 1, 0, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_repeat_info(NULL, 1, 1), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_keyboard_layout(NULL, (const uint8_t *)"00000409", 8), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, NULL, 8), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_release_all(NULL), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_disconnect(NULL), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_translate_key(30, NULL), GuestInputNullArgument);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    EXPECT_STATUS(guest_input_release_all(&state), GuestInputOk);
}

/** @brief Scenario: the typed mapping equals av_input_translate_key() for every code, and the repeat policy equals av_input_key_repeatable(). */
static void test_mapping_equivalence(void) {
    uint32_t code;
    for (code = 0; code <= 768u; code++) {
        guest_input_translation_t guest;
        av_key_translation_t av = av_input_translate_key(code);
        EXPECT_STATUS(guest_input_translate_key(code, &guest), GuestInputOk);
        switch (av.kind) {
        case AvKeyTranslationUnsupported:
            CHECK(guest.kind == GuestInputKeyUnsupported);
            break;
        case AvKeyTranslationScanCode:
            CHECK(guest.kind == GuestInputKeyScanCode);
            CHECK(guest.scan_code == av.scan_code);
            CHECK(guest.extended == av.extended);
            CHECK(guest.virtual_key == 0);
            break;
        case AvKeyTranslationVirtualKey:
            CHECK(guest.kind == GuestInputKeyVirtualKey);
            CHECK(guest.virtual_key == av.virtual_key);
            CHECK(guest.scan_code == 0 && guest.extended == 0);
            break;
        case AvKeyTranslationPause:
            CHECK(guest.kind == GuestInputKeyPause);
            CHECK(guest.virtual_key == GuestInputVkPause && guest.scan_code == GuestInputScanPause);
            CHECK(av.pause_make[0] == 0xe1 && av.pause_break[0] == 0xe1);
            break;
        case AvKeyTranslationSuppressed:
            CHECK(guest.kind == GuestInputKeySuppressed);
            break;
        default:
            CHECK(0);
        }
        CHECK(guest_input_key_repeatable(code) == av_input_key_repeatable(code));
    }
    CHECK(guest_input_key_repeatable(0xFFFFFFFFu) == av_input_key_repeatable(0xFFFFFFFFu));
}

/** @brief Scenario: focus enter, lock sync, scan-code and virtual-key injection forms. */
static void test_frame_injection_forms(void) {
    guest_input_t state;
    const expected_t plain[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE},
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x1c, KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY},
        {0, 0x1c, KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP},
        {0x15, 0, 0},
        {0x15, 0, KEYEVENTF_KEYUP},
        {0x13, 0x45, 0},
        {0x13, 0x45, KEYEVENTF_KEYUP},
        {0, 0x64, KEYEVENTF_SCANCODE},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    CHECK(g_record_count == 0);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 96, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 96, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 122, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 122, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 119, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 119, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 183, 1), GuestInputOk);
    expect_records(plain, 9);
    CHECK(state.held_count == 1);
    records_clear();
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 1 && g_records[0].scan == 0x64 && g_records[0].flags == (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
    records_clear();
}

/** @brief Scenario: lock sync plans before any other key, and frames during sync are queued in order. */
static void test_lock_sync_order(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(1, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    CHECK(state.lock_pending);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputQueued);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 0), GuestInputQueued);
    CHECK(state.queue_count == 2);
    CHECK(g_record_count == 0);
    /* Desired Num on, Caps on at the host; the guest has Caps on: only Num differs. */
    set_locks(1, 0, 0);
    EXPECT_STATUS(locks(&state, lock_mask(1, 1, 0)), GuestInputOk);
    expected_t num_only[] = {{0x90, 0, 0}, {0x90, 0, KEYEVENTF_KEYUP}, {0, 0x1e, KEYEVENTF_SCANCODE}, {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP}};
    expect_records(num_only, 4);
    CHECK(!state.lock_pending && state.queue_count == 0 && state.held_count == 0);
    records_clear();
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 0);
}

/** @brief Scenario: the lock plan toggles every differing lock, in Caps, Num, Scroll order, press then release. */
static void test_lock_plan_all_bits(void) {
    guest_input_t state;
    const expected_t all_three[] = {
        {0x14, 0, 0}, {0x14, 0, KEYEVENTF_KEYUP},
        {0x90, 0, 0}, {0x90, 0, KEYEVENTF_KEYUP},
        {0x91, 0, 0}, {0x91, 0, KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    set_locks(0, 0, 0);
    EXPECT_STATUS(locks(&state, 7), GuestInputOk);
    expect_records(all_three, 6);
    records_clear();
    /* Already equal: an empty plan, no records. Scroll stays as the host reports it. */
    set_locks(1, 1, 1);
    EXPECT_STATUS(locks(&state, 7), GuestInputOk);
    CHECK(g_record_count == 0);
    /* Wrong mask bit rejected as protocol violation (terminal). */
    EXPECT_STATUS(guest_input_lock_state(&state, next_serial(), 8, state.epoch), GuestInputProtocolViolation);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputTerminal);
}

/** @brief Scenario: the lock queue bound holds 64 entries and the 65th is terminal with no injection. */
static void test_lock_queue_overflow(void) {
    guest_input_t state;
    size_t index;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    for (index = 0; index < GuestInputPendingCapacity; index++) {
        EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, index % 2 == 0), GuestInputQueued);
    }
    CHECK(state.queue_count == GuestInputPendingCapacity);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputQueueOverflow);
    CHECK(state.terminal);
    CHECK(state.queue_count == 0 && g_record_count == 0);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: a release frame queued during sync runs after the queued press, so the held key is released in order. */
static void test_release_queued_during_sync(void) {
    guest_input_t state;
    const expected_t sequence[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE},
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputQueued);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, next_serial(), state.epoch), GuestInputQueued);
    CHECK(g_record_count == 0 && state.queue_count == 2);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    expect_records(sequence, 2);
    CHECK(state.held_count == 0 && state.queue_count == 0 && !state.lock_pending);
    records_clear();
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 0);
}

/** @brief Scenario: lock state without an active pair is a consumed no-op; a planner serial gap of half the serial space is a terminal violation. */
static void test_lock_state_edges(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(1, 0, 0);
    EXPECT_STATUS(locks(&state, 0), GuestInputIgnored);
    CHECK(state.serial == 1 && g_record_count == 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    CHECK(state.serial == 2);
    /* First planner update is accepted (no prior update in this epoch). Caps already matches: empty plan. */
    EXPECT_STATUS(guest_input_lock_state(&state, 3, 1, state.epoch), GuestInputOk);
    CHECK(g_record_count == 0);
    records_clear();
    /* Planner serial gate: 3 then 0x80000004 is not wrap-newer (gap is 2^31 + 1), so the planner refuses. */
    EXPECT_STATUS(guest_input_lock_state(&state, 0x80000004u, 1, state.epoch), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: serial rules. Zero, replay, regression and wrap are each terminal. */
static void test_serial_rules(void) {
    guest_input_t state;
    fake_reset();
    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 0, 1, 1), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 5, 1, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, 5, 1), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 5, 1, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_key(&state, WindowA, GenerationA, 4, 1, 30, 1), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 0xFFFFFFFFu, 1, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_key(&state, WindowA, GenerationA, 1, 1, 30, 1), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: stale epoch frames consume their serial and have no effect; a newer or zero epoch is terminal. */
static void test_epoch_rules(void) {
    guest_input_t state;
    uint64_t next = 0;
    const expected_t revoke_release[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x30, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 48, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_revoke(&state, &next), GuestInputOk);
    CHECK(next == 2 && state.epoch == 2 && !state.pair_active && state.held_count == 0);
    expect_records(revoke_release, 2);
    records_clear();

    /* A current focus-on at epoch 2 activates the pair. Then stale epoch-1 frames consume serials only. */
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_key(&state, WindowA, GenerationA, next_serial(), 1, 30, 0), GuestInputStaleEpoch);
    EXPECT_STATUS(guest_input_key(&state, WindowA, GenerationA, next_serial(), 1, 48, 1), GuestInputStaleEpoch);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, next_serial(), 1, 0), GuestInputStaleEpoch);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, next_serial(), 1), GuestInputStaleEpoch);
    CHECK(state.pair_active && state.held_count == 1 && g_record_count == 0);
    /* A stale serial replayed as a current frame is a regression: terminal. */
    EXPECT_STATUS(guest_input_key(&state, WindowA, GenerationA, g_serial - 2, 2, 30, 0), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 1);
    records_clear();

    /* A newer epoch and a zero epoch are terminal. */
    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 1, 2, 1), GuestInputProtocolViolation);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    session_start(&state);
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, 1, 0, 1), GuestInputProtocolViolation);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: a stale focus-off cannot release the current pair's held keys. */
static void test_stale_focus_off_is_inert(void) {
    guest_input_t state;
    uint64_t next = 0;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(guest_input_revoke(&state, &next), GuestInputOk);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_focus(&state, WindowA, GenerationA, next_serial(), 1, 0), GuestInputStaleEpoch);
    CHECK(state.pair_active && state.held_count == 1 && g_record_count == 0);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 1);
    records_clear();
}

/** @brief Scenario: pair transitions. Enter on a new pair releases the old pair's keys first. Leave on the active pair releases. Other pairs are no-ops. */
static void test_focus_transitions(void) {
    guest_input_t state;
    const expected_t first_release[] = {{0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP}};
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputIgnored);
    CHECK(g_record_count == 0);
    EXPECT_STATUS(focus(&state, WindowB, GenerationB, 0), GuestInputIgnored);
    EXPECT_STATUS(focus(&state, WindowB, GenerationB, 1), GuestInputOk);
    expect_records(first_release, 1);
    CHECK(state.lock_pending && state.pair_window == WindowB && state.pair_generation == GenerationB);
    records_clear();
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputIgnored);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, next_serial(), state.epoch), GuestInputIgnored);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 0), GuestInputIgnored);
    EXPECT_STATUS(focus(&state, WindowB, GenerationB, 0), GuestInputOk);
    CHECK(!state.pair_active && !state.lock_pending && state.queue_count == 0 && g_record_count == 0);
    EXPECT_STATUS(focus(&state, WindowB, GenerationB, 0), GuestInputIgnored);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: key rules. Key-up without a held key is ignored; repeats follow the policy; suppressed and unsupported codes are never injected; out-of-range codes are terminal. */
static void test_key_rules(void) {
    guest_input_t state;
    const expected_t sequence[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE},
        {0, 0x1e, KEYEVENTF_SCANCODE},
        {0, 0x2a, KEYEVENTF_SCANCODE},
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x2a, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 0), GuestInputIgnored);
    CHECK(g_record_count == 0);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    /* Repeatable key held: a second down is injected as a plain down with no intervening up. */
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    /* Left Shift (evdev 42) is not repeatable: its first down is injected, a second down is not. */
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 42, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 42, 1), GuestInputNotInjected);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 42, 0), GuestInputOk);
    expect_records(sequence, 5);
    CHECK(state.held_count == 0);
    records_clear();
    /* Suppressed and unsupported codes: no injection, not an error. */
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 116, 1), GuestInputNotInjected);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 142, 1), GuestInputNotInjected);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 200, 1), GuestInputNotInjected);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 116, 0), GuestInputNotInjected);
    CHECK(g_record_count == 0 && state.held_count == 0);
    /* Key frames for another pair are no-ops. */
    EXPECT_STATUS(key(&state, WindowB, GenerationB, 30, 1), GuestInputIgnored);
    CHECK(g_record_count == 0);
    /* Out-of-range codes are malformed: terminal. */
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 0, 1), GuestInputProtocolViolation);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    session_start(&state);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 768, 1), GuestInputProtocolViolation);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: InputRelease releases every held key of the pair, keeps the pair, and rejects other pairs. */
static void test_release_frame(void) {
    guest_input_t state;
    const expected_t sequence[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x30, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 48, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_release(&state, WindowB, GenerationB, next_serial(), state.epoch), GuestInputIgnored);
    CHECK(g_record_count == 0);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, next_serial(), state.epoch), GuestInputOk);
    expect_records(sequence, 2);
    CHECK(state.pair_active && state.held_count == 0);
    records_clear();
    EXPECT_STATUS(guest_input_release(&state, WindowA, 0, next_serial(), state.epoch), GuestInputProtocolViolation);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: revocation releases held keys in ascending code order, then reserves the next epoch and clears the pair. */
static void test_revoke_release_order(void) {
    guest_input_t state;
    uint64_t next = 0;
    const expected_t sequence[] = {
        {0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x30, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
        {0, 0x64, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP},
    };
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 183, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 48, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_revoke(&state, &next), GuestInputOk);
    expect_records(sequence, 3);
    CHECK(next == 2 && state.epoch == 2 && !state.pair_active && !state.lock_pending && state.held_count == 0);
    records_clear();
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(g_record_count == 0);
}

/** @brief Scenario: epoch exhaustion is terminal, and a failed release during revocation is terminal with no epoch reserved. */
static void test_revoke_failures(void) {
    guest_input_t state;
    uint64_t next = 0;
    fake_reset();
    session_start(&state);
    state.epoch = UINT64_MAX;
    EXPECT_STATUS(guest_input_revoke(&state, &next), GuestInputEpochExhausted);
    CHECK(state.terminal && state.epoch == UINT64_MAX);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    g_fail_all_sends = 1;
    EXPECT_STATUS(guest_input_revoke(&state, &next), GuestInputReleaseFailed);
    CHECK(state.terminal && state.epoch == 1 && state.held_count == 1);
    g_fail_all_sends = 0;
    records_clear();
    EXPECT_STATUS(guest_input_release_all(&state), GuestInputOk);
    CHECK(state.held_count == 0 && g_record_count == 1);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: release-all on focus leave fails terminally when a release fails; disconnect and release-all retry the retained bits. */
static void test_release_failure_on_leave(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    g_fail_all_sends = 1;
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 0), GuestInputReleaseFailed);
    CHECK(state.terminal && state.held_count == 1);
    g_fail_all_sends = 0;
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(state.held_count == 0);
}

/** @brief Scenario: an injection failure is terminal and attempts cleanup of the other held keys. */
static void test_injection_failure(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 48, 1), GuestInputOk);
    g_fail_send_call = g_send_calls + 1;
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputInjectionFailed);
    CHECK(state.terminal && state.held_count == 0);
    CHECK(g_records[g_record_count - 1].flags == (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
    g_fail_send_call = 0;
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 0), GuestInputTerminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: injection failure of a lock toggle is terminal. */
static void test_lock_injection_failure(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    set_locks(0, 0, 0);
    g_fail_send_call = g_send_calls + 1;
    EXPECT_STATUS(locks(&state, 1), GuestInputInjectionFailed);
    CHECK(state.terminal);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: queued key entries that fail during the drain are terminal. */
static void test_drain_failure(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputQueued);
    /* The lock plan is empty, so the first SendInput of the drain is the key. */
    g_fail_send_call = g_send_calls + 1;
    EXPECT_STATUS(locks(&state, 0), GuestInputInjectionFailed);
    CHECK(state.terminal && state.held_count == 0);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);

    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputQueued);
    g_fail_all_sends = 1;
    EXPECT_STATUS(locks(&state, 0), GuestInputInjectionFailed);
    g_fail_all_sends = 0;
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: disconnect releases in ascending order and latches terminal; later frames are refused; release-all stays allowed. */
static void test_disconnect(void) {
    guest_input_t state;
    const expected_t sequence[] = {{0, 0x1e, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP}};
    fake_reset();
    g_serial = 0;
    session_start(&state);
    set_locks(0, 0, 0);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    EXPECT_STATUS(locks(&state, 0), GuestInputOk);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputOk);
    records_clear();
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    expect_records(sequence, 1);
    CHECK(state.terminal);
    records_clear();
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputTerminal);
    EXPECT_STATUS(key(&state, WindowA, GenerationA, 30, 1), GuestInputTerminal);
    EXPECT_STATUS(guest_input_release(&state, WindowA, GenerationA, next_serial(), state.epoch), GuestInputTerminal);
    EXPECT_STATUS(locks(&state, 0), GuestInputTerminal);
    EXPECT_STATUS(guest_input_repeat_info(&state, 500, 30), GuestInputTerminal);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"00000409", 8), GuestInputTerminal);
    EXPECT_STATUS(guest_input_admit_serial(&state, next_serial(), 1), GuestInputTerminal);
    CHECK(g_record_count == 0);
    EXPECT_STATUS(guest_input_release_all(&state), GuestInputOk);
}

/** @brief Scenario: frames without a serial never consume one and never change the lease. */
static void test_serial_free_frames(void) {
    guest_input_t state;
    fake_reset();
    g_serial = 0;
    session_start(&state);
    EXPECT_STATUS(guest_input_repeat_info(&state, 500, 30), GuestInputOk);
    EXPECT_STATUS(guest_input_repeat_info(&state, 10001, 30), GuestInputInvalidArgument);
    EXPECT_STATUS(guest_input_repeat_info(&state, 500, 1001), GuestInputInvalidArgument);
    CHECK(state.repeat_known && state.repeat_interval_ms == 500 && state.repeat_rate == 30);
    CHECK(state.serial == 0 && state.epoch == 1);
    EXPECT_STATUS(focus(&state, WindowA, GenerationA, 1), GuestInputOk);
    CHECK(state.serial == 1);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: KeyboardLayout validation, unchanged, no-window, load failure, post failure and success. */
static void test_keyboard_layout(void) {
    guest_input_t state;
    fake_reset();
    session_start(&state);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040", 7), GuestInputInvalidArgument);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"00000000", 8), GuestInputInvalidArgument);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040G", 8), GuestInputInvalidArgument);
    CHECK(g_load_calls == 0 && !state.layout_known);

    g_foreground = NULL;
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040c", 8), GuestInputLayoutNoWindow);
    CHECK(g_load_calls == 0 && !state.layout_known);

    g_foreground = g_window_handle;
    g_thread_id = 0;
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040c", 8), GuestInputLayoutNoWindow);
    CHECK(g_load_calls == 0);
    g_thread_id = ThreadId;

    g_load_fail = 1;
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040c", 8), GuestInputLayoutLoadFailed);
    CHECK(!state.layout_known && g_load_calls == 1 && g_post_count == 0);
    g_load_fail = 0;

    g_post_result = 0;
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040c", 8), GuestInputLayoutPostFailed);
    CHECK(!state.layout_known && g_post_count == 1);
    g_post_result = 1;

    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040c", 8), GuestInputOk);
    CHECK(state.layout_known && g_load_calls == 3);
    CHECK(wcscmp(g_last_klid, L"0000040C") == 0);
    CHECK(g_last_load_flags == (KLF_ACTIVATE | KLF_SETFORPROCESS));
    CHECK(g_post_count == 2 && g_posts[1].window == g_window_handle && g_posts[1].message == WM_INPUTLANGCHANGEREQUEST);
    CHECK(g_posts[1].wparam == 0 && g_posts[1].lparam == (LPARAM)(uintptr_t)g_layout_handle);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"0000040C", 8), GuestInputLayoutUnchanged);
    CHECK(g_load_calls == 3 && g_post_count == 2);
    EXPECT_STATUS(guest_input_keyboard_layout(&state, (const uint8_t *)"00000409", 8), GuestInputOk);
    CHECK(wcscmp(g_last_klid, L"00000409") == 0 && g_load_calls == 4);
    CHECK(state.serial == 0);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: admit hook. Current accepted, stale consumed, regression and future terminal. */
static void test_admit_hook(void) {
    guest_input_t state;
    fake_reset();
    session_start(&state);
    EXPECT_STATUS(guest_input_admit_serial(&state, 1, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_admit_serial(&state, 2, 0), GuestInputProtocolViolation);
    session_start(&state);
    EXPECT_STATUS(guest_input_admit_serial(&state, 1, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_admit_serial(&state, 3, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_admit_serial(&state, 2, 1), GuestInputProtocolViolation);
    session_start(&state);
    EXPECT_STATUS(guest_input_admit_serial(&state, 4, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_admit_serial(&state, 5, 0), GuestInputProtocolViolation);
    session_start(&state);
    EXPECT_STATUS(guest_input_admit_serial(&state, 1, 2), GuestInputProtocolViolation);
    session_start(&state);
    EXPECT_STATUS(guest_input_admit_serial(&state, 6, 1), GuestInputOk);
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
}

/** @brief Scenario: the randomized sequence. Each seed drives valid frames only; the injected stream must never release an up-key that is not down, and must end with no key held. */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t rng_next(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

/** @brief Identity of one injected record for the balance check. */
static uint32_t record_state_index(const fake_record_t *record) {
    uint32_t extended = (record->flags & KEYEVENTF_EXTENDEDKEY) ? 1u : 0u;
    return ((uint32_t)record->vk << 9) | ((uint32_t)record->scan << 1) | extended;
}

static void test_randomized_sequence(void) {
    static const uint32_t Codes[] = {30, 48, 42, 183, 116, 142, 96, 28, 105, 113, 122, 119, 1, 58, 100, 29, 97, 125, 200};
    static const uint64_t Windows[] = {WindowA, WindowB, WindowA + 1};
    static const uint64_t Generations[] = {GenerationA, GenerationB, 3};
    static uint8_t down[1u << StateIndexBits];
    guest_input_t state;
    size_t event;
    size_t index;
    size_t up_before_down = 0;
    size_t downs = 0;
    size_t repeats = 0;
    uint64_t next_epoch = 0;

    fake_reset();
    g_serial = 0;
    session_start(&state);
    g_rng = 0x9E3779B97F4A7C15ull;
    memset(down, 0, sizeof down);
    records_clear();
    for (event = 0; event < 2000; event++) {
        uint32_t choice = rng_next() % 100;
        /* Seventy percent of frames name the first pair so that the active-pair paths run often. */
        uint64_t window = rng_next() % 10u < 7u ? WindowA : Windows[rng_next() % 3];
        uint64_t generation = rng_next() % 10u < 7u ? GenerationA : Generations[rng_next() % 3];
        /* One frame in four is key 30 (repeatable), so typematic repeats occur in the stream. */
        uint32_t code = rng_next() % 4u == 0 ? 30u : Codes[rng_next() % (sizeof Codes / sizeof Codes[0])];
        uint8_t pressed = (uint8_t)(rng_next() & 1u);
        guest_input_status_t status;
        if (choice < 4 && state.epoch < 1000) {
            status = guest_input_revoke(&state, &next_epoch);
            CHECK(status == GuestInputOk);
        } else if (choice < 14) {
            status = focus(&state, window, generation, 1);
        } else if (choice < 20) {
            status = focus(&state, window, generation, 0);
        } else if (choice < 30) {
            status = locks(&state, (uint8_t)(rng_next() % 8u));
            set_locks((int)(rng_next() & 1u), (int)(rng_next() & 1u), (int)(rng_next() & 1u));
        } else if (choice < 36) {
            status = guest_input_release(&state, window, generation, next_serial(), state.epoch);
        } else if (choice < 40 && state.epoch >= 2) {
            status = guest_input_key(&state, window, generation, next_serial(), state.epoch - 1, code, pressed);
            CHECK(status == GuestInputStaleEpoch);
            continue;
        } else if (choice < 46) {
            status = guest_input_keyboard_layout(&state, (const uint8_t *)(rng_next() & 1u ? "00000409" : "0000040C"), 8);
            CHECK(status == GuestInputOk || status == GuestInputLayoutUnchanged);
            continue;
        } else if (choice < 50) {
            status = guest_input_repeat_info(&state, rng_next() % 10000u, rng_next() % 1000u);
            CHECK(status == GuestInputOk);
            continue;
        } else {
            status = key(&state, window, generation, code, pressed);
        }
        CHECK(status != GuestInputProtocolViolation && status != GuestInputInjectionFailed &&
            status != GuestInputTerminal && status != GuestInputQueueOverflow &&
            status != GuestInputReleaseFailed && status != GuestInputEpochExhausted);
    }
    /* Disconnect releases whatever the random walk left down; the stream is then checked as a whole.
     * An up for a key that is not down is a violation. A down for a key that is already down is a
     * repeat (allowed for the repeatable policy). Every down run must end in an up. */
    EXPECT_STATUS(guest_input_disconnect(&state), GuestInputOk);
    CHECK(state.held_count == 0);
    for (index = 0; index < g_record_count; index++) {
        uint32_t id = record_state_index(&g_records[index]);
        int is_up = (g_records[index].flags & KEYEVENTF_KEYUP) != 0;
        CHECK(id < (1u << StateIndexBits));
        if (is_up) {
            if (!down[id]) up_before_down++;
            down[id] = 0;
        } else {
            if (down[id]) repeats++;
            else downs++;
            down[id] = 1;
        }
    }
    CHECK(up_before_down == 0);
    for (index = 0; index < (1u << StateIndexBits); index++) CHECK(down[index] == 0);
    CHECK(downs > 0 && g_record_count > 0);
    printf("randomized: %zu events, %zu records, %zu key runs, %zu repeat downs\n", event, g_record_count, downs, repeats);
    records_clear();
}

int main(void) {
    test_init_and_null_arguments();
    test_mapping_equivalence();
    test_frame_injection_forms();
    test_lock_sync_order();
    test_lock_plan_all_bits();
    test_lock_queue_overflow();
    test_release_queued_during_sync();
    test_lock_state_edges();
    test_serial_rules();
    test_epoch_rules();
    test_stale_focus_off_is_inert();
    test_focus_transitions();
    test_key_rules();
    test_release_frame();
    test_revoke_release_order();
    test_revoke_failures();
    test_release_failure_on_leave();
    test_injection_failure();
    test_lock_injection_failure();
    test_drain_failure();
    test_disconnect();
    test_serial_free_frames();
    test_keyboard_layout();
    test_admit_hook();
    test_randomized_sequence();
    printf("guest_input_test: all checks passed\n");
    return 0;
}
