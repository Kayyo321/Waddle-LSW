/**
 * @file guest_input.c
 * @brief Guest keyboard executor: lease, serial, lock sync, layout sync and release rules.
 *
 * The typed mapping and the repeat policy are reproduced from src/av/av_input.c, not linked
 * from it. av_input.c depends on the AV protocol codec (av_control_encode), which the guest
 * agent does not carry. tests/interop/guest_input_test.c proves equivalence over every code 0..768.
 */
#include "guest_input.h"

#include <string.h>

/** @brief Largest KeyboardLayout byte count the wide copy can hold, plus the terminator. */
enum { GuestInputWideKlidChars = GuestInputKlidBytes + 1 };

/** @brief Typed mapping row. Same content as av_input.c KeyMappings. */
typedef struct guest_input_mapping_t {
    uint16_t key;
    guest_input_key_kind_t kind;
    uint8_t scan_code;
    uint8_t extended;
    uint16_t virtual_key;
} guest_input_mapping_t;

/** @brief Frozen evdev-to-Windows table: codes outside 1..83 and not listed here are unsupported. */
static const guest_input_mapping_t GuestInputMappings[] = {
    {85, GuestInputKeyScanCode, 0x29, 0, 0},
    {86, GuestInputKeyScanCode, 0x56, 0, 0},
    {87, GuestInputKeyScanCode, 0x57, 0, 0},
    {88, GuestInputKeyScanCode, 0x58, 0, 0},
    {89, GuestInputKeyScanCode, 0x73, 0, 0},
    {90, GuestInputKeyScanCode, 0x70, 0, 0},
    {92, GuestInputKeyScanCode, 0x79, 0, 0},
    {93, GuestInputKeyScanCode, 0x70, 0, 0},
    {94, GuestInputKeyScanCode, 0x7b, 0, 0},
    {95, GuestInputKeyScanCode, 0x5c, 0, 0},
    {96, GuestInputKeyScanCode, 0x1c, 1, 0},
    {97, GuestInputKeyScanCode, 0x1d, 1, 0},
    {98, GuestInputKeyScanCode, 0x35, 1, 0},
    {99, GuestInputKeyScanCode, 0x37, 1, 0},
    {100, GuestInputKeyScanCode, 0x38, 1, 0},
    {102, GuestInputKeyScanCode, 0x47, 1, 0},
    {103, GuestInputKeyScanCode, 0x48, 1, 0},
    {104, GuestInputKeyScanCode, 0x49, 1, 0},
    {105, GuestInputKeyScanCode, 0x4b, 1, 0},
    {106, GuestInputKeyScanCode, 0x4d, 1, 0},
    {107, GuestInputKeyScanCode, 0x4f, 1, 0},
    {108, GuestInputKeyScanCode, 0x50, 1, 0},
    {109, GuestInputKeyScanCode, 0x51, 1, 0},
    {110, GuestInputKeyScanCode, 0x52, 1, 0},
    {111, GuestInputKeyScanCode, 0x53, 1, 0},
    {113, GuestInputKeyScanCode, 0x20, 1, 0},
    {114, GuestInputKeyScanCode, 0x2e, 1, 0},
    {115, GuestInputKeyScanCode, 0x30, 1, 0},
    {116, GuestInputKeySuppressed, 0, 0, 0},
    {117, GuestInputKeyScanCode, 0x59, 0, 0},
    {119, GuestInputKeyPause, 0x45, 0, GuestInputVkPause},
    {121, GuestInputKeyScanCode, 0x7e, 0, 0},
    {122, GuestInputKeyVirtualKey, 0, 0, 0x15},
    {123, GuestInputKeyVirtualKey, 0, 0, 0x19},
    {124, GuestInputKeyScanCode, 0x7d, 0, 0},
    {125, GuestInputKeyScanCode, 0x5b, 1, 0},
    {126, GuestInputKeyScanCode, 0x5c, 1, 0},
    {127, GuestInputKeyScanCode, 0x5d, 1, 0},
    {128, GuestInputKeyScanCode, 0x68, 1, 0},
    {140, GuestInputKeyScanCode, 0x21, 1, 0},
    {142, GuestInputKeySuppressed, 0, 0, 0},
    {150, GuestInputKeyScanCode, 0x32, 1, 0},
    {155, GuestInputKeyScanCode, 0x6c, 1, 0},
    {156, GuestInputKeyScanCode, 0x66, 1, 0},
    {158, GuestInputKeyScanCode, 0x6a, 1, 0},
    {159, GuestInputKeyScanCode, 0x69, 1, 0},
    {163, GuestInputKeyScanCode, 0x19, 1, 0},
    {164, GuestInputKeyScanCode, 0x22, 1, 0},
    {165, GuestInputKeyScanCode, 0x10, 1, 0},
    {166, GuestInputKeyScanCode, 0x24, 1, 0},
    {172, GuestInputKeyScanCode, 0x32, 1, 0},
    {173, GuestInputKeyScanCode, 0x67, 1, 0},
    {183, GuestInputKeyScanCode, 0x64, 0, 0},
    {184, GuestInputKeyScanCode, 0x65, 0, 0},
    {185, GuestInputKeyScanCode, 0x66, 0, 0},
    {186, GuestInputKeyScanCode, 0x67, 0, 0},
    {187, GuestInputKeyScanCode, 0x68, 0, 0},
    {188, GuestInputKeyScanCode, 0x69, 0, 0},
    {189, GuestInputKeyScanCode, 0x6a, 0, 0},
    {190, GuestInputKeyScanCode, 0x6b, 0, 0},
    {191, GuestInputKeyScanCode, 0x6c, 0, 0},
    {192, GuestInputKeyScanCode, 0x6d, 0, 0},
    {193, GuestInputKeyScanCode, 0x6e, 0, 0},
    {194, GuestInputKeyScanCode, 0x76, 0, 0},
    {217, GuestInputKeyScanCode, 0x65, 1, 0},
    {226, GuestInputKeyScanCode, 0x6d, 1, 0},
};

/** @brief Keys that never auto-repeat (modifiers, lock keys, Pause, suppressed keys). Same set as av_input.c NonRepeatKeys. */
static const uint16_t GuestInputNonRepeatKeys[] = {29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 122, 123, 125, 126, 142};

/**
 * @brief Whether the executor may inject `key` at all.
 * @param key in, evdev code. @return 1 for injectable codes (scan, VK, Pause), 0 otherwise.
 */
static int key_is_injectable(uint32_t key) {
    guest_input_translation_t translation;
    if (guest_input_translate_key(key, &translation) != GuestInputOk) return 0;
    return translation.kind == GuestInputKeyScanCode || translation.kind == GuestInputKeyVirtualKey ||
        translation.kind == GuestInputKeyPause;
}

/**
 * @brief Test one bit of the held set.
 * @param state in. @param key in, 1..767. @return true when the key is held.
 */
static bool held_test(const guest_input_t *state, uint32_t key) {
    return ((state->held[key >> 3] >> (key & 7u)) & 1u) != 0;
}

/**
 * @brief Set one held bit and count it.
 * @param state inout. @param key in, 1..767; must not be held yet.
 */
static void held_set(guest_input_t *state, uint32_t key) {
    state->held[key >> 3] = (uint8_t)(state->held[key >> 3] | (1u << (key & 7u)));
    state->held_count++;
}

/**
 * @brief Clear one held bit and uncount it.
 * @param state inout. @param key in, 1..767; must be held.
 */
static void held_clear(guest_input_t *state, uint32_t key) {
    state->held[key >> 3] = (uint8_t)(state->held[key >> 3] & ~(1u << (key & 7u)));
    state->held_count--;
}

/**
 * @brief Send one INPUT record through the table.
 * @param state in. @param input in, nonnull, one record.
 * @return 0 when SendInput inserted the record, -1 otherwise.
 */
static int send_record(const guest_input_t *state, INPUT *input) {
    return state->win32->send_input(1, input, (int)sizeof *input) == 1 ? 0 : -1;
}

/**
 * @brief Inject one keyboard record for a supported evdev key, in the form AV uses.
 * @param state in. @param key in, injectable code. @param pressed in, true for a press.
 * @return 0 inserted, -1 failure or non-injectable key.
 */
static int inject_key(const guest_input_t *state, uint32_t key, bool pressed) {
    guest_input_translation_t translation;
    INPUT input;
    DWORD up = pressed ? 0u : KEYEVENTF_KEYUP;
    if (guest_input_translate_key(key, &translation) != GuestInputOk) return -1;
    memset(&input, 0, sizeof input);
    input.type = INPUT_KEYBOARD;
    switch (translation.kind) {
    case GuestInputKeyScanCode:
        input.ki.wScan = translation.scan_code;
        input.ki.dwFlags = up | KEYEVENTF_SCANCODE | (translation.extended ? KEYEVENTF_EXTENDEDKEY : 0u);
        break;
    case GuestInputKeyVirtualKey:
        input.ki.wVk = translation.virtual_key;
        input.ki.dwFlags = up;
        break;
    case GuestInputKeyPause:
        input.ki.wVk = VK_PAUSE;
        input.ki.wScan = GuestInputScanPause;
        input.ki.dwFlags = up;
        break;
    default:
        return -1;
    }
    return send_record(state, &input);
}

/**
 * @brief Inject one virtual-key record for a lock toggle (VK_CAPITAL, VK_NUMLOCK or VK_SCROLL).
 * @param state in. @param vk in, virtual-key code. @param pressed in, true for a press.
 * @return 0 inserted, -1 failure.
 */
static int inject_vk(const guest_input_t *state, uint16_t vk, bool pressed) {
    INPUT input;
    memset(&input, 0, sizeof input);
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = pressed ? 0u : KEYEVENTF_KEYUP;
    return send_record(state, &input);
}

/**
 * @brief Latch the terminal state, discard queued work and release held keys (best effort).
 * @param state inout. @param status in, the failure being reported; returned unchanged.
 * @return status.
 */
static guest_input_status_t fail(guest_input_t *state, guest_input_status_t status) {
    state->terminal = true;
    state->lock_pending = false;
    state->queue_count = 0;
    (void)guest_input_release_all(state);
    return status;
}

/**
 * @brief Whether a frame names the active pair.
 * @param state in. @param window in. @param generation in. @return true when the pair is active and matches.
 */
static bool pair_matches(const guest_input_t *state, uint64_t window, uint64_t generation) {
    return state->pair_active && state->pair_window == window && state->pair_generation == generation;
}

/**
 * @brief Release every held key in ascending order. Failed keys keep their bit.
 * @param state inout. @return GuestInputOk, or GuestInputReleaseFailed when any release failed.
 */
static guest_input_status_t release_held(guest_input_t *state) {
    bool failed = false;
    for (uint32_t key = 1; key <= GuestInputKeyMax; key++) {
        if (!held_test(state, key)) continue;
        if (inject_key(state, key, false) != 0) {
            failed = true;
            continue;
        }
        held_clear(state, key);
    }
    return failed ? GuestInputReleaseFailed : GuestInputOk;
}

/**
 * @brief Apply one key transition to the injection and held state.
 * @param state inout. @param key in, evdev code 1..767. @param pressed in, true for a down.
 * @return GuestInputOk, GuestInputNotInjected, GuestInputIgnored, or a terminal status.
 */
static guest_input_status_t apply_key(guest_input_t *state, uint32_t key, bool pressed) {
    bool held = held_test(state, key);
    if (!key_is_injectable(key)) return GuestInputNotInjected;
    if (pressed) {
        if (held && !guest_input_key_repeatable(key)) return GuestInputNotInjected;
        if (inject_key(state, key, true) != 0) return fail(state, GuestInputInjectionFailed);
        if (!held) held_set(state, key);
        return GuestInputOk;
    }
    if (!held) return GuestInputIgnored;
    if (inject_key(state, key, false) != 0) return fail(state, GuestInputInjectionFailed);
    held_clear(state, key);
    return GuestInputOk;
}

/**
 * @brief Apply the queued lock-sync entries in arrival order, then empty the queue.
 * @param state inout. @return GuestInputOk, or the terminal status that stopped the drain.
 */
static guest_input_status_t drain_queue(guest_input_t *state) {
    for (uint8_t index = 0; index < state->queue_count; index++) {
        const guest_input_queued_t entry = state->queue[index];
        guest_input_status_t status = GuestInputOk;
        if (entry.is_release) {
            status = release_held(state);
            if (status != GuestInputOk) return fail(state, GuestInputReleaseFailed);
        } else {
            status = apply_key(state, entry.key, entry.pressed);
            if (status == GuestInputInjectionFailed) return status;
        }
    }
    state->queue_count = 0;
    return GuestInputOk;
}

/**
 * @brief Append one entry to the lock-sync queue. Overflow is terminal.
 * @param state inout. @param entry in. @return GuestInputQueued, or GuestInputQueueOverflow (terminal).
 */
static guest_input_status_t enqueue(guest_input_t *state, guest_input_queued_t entry) {
    if (state->queue_count >= GuestInputPendingCapacity) return fail(state, GuestInputQueueOverflow);
    state->queue[state->queue_count] = entry;
    state->queue_count++;
    return GuestInputQueued;
}

/**
 * @brief Read the current toggle state of the three lock keys as a lock mask.
 * @param state in. @return mask with WaddleKeyboardLockCaps, Num and Scroll bits set when toggled on.
 */
static uint8_t current_lock_bits(const guest_input_t *state) {
    uint8_t bits = 0;
    if ((state->win32->get_key_state(VK_CAPITAL) & 1) != 0) bits |= WaddleKeyboardLockCaps;
    if ((state->win32->get_key_state(VK_NUMLOCK) & 1) != 0) bits |= WaddleKeyboardLockNum;
    if ((state->win32->get_key_state(VK_SCROLL) & 1) != 0) bits |= WaddleKeyboardLockScroll;
    return bits;
}

guest_input_status_t guest_input_init(guest_input_t *state, const guest_input_win32_t *win32) {
    if (!state || !win32) return GuestInputNullArgument;
    if (!win32->send_input || !win32->get_key_state || !win32->load_keyboard_layout || !win32->post_message ||
        !win32->get_foreground_window || !win32->get_window_thread_process_id) return GuestInputNullArgument;
    memset(state, 0, sizeof *state);
    state->win32 = win32;
    return GuestInputOk;
}

guest_input_status_t guest_input_admit_serial(guest_input_t *state, uint32_t serial, uint64_t lease) {
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    /* A serial that is zero, not greater than the last accepted one (this includes a wrap back to a
     * low value), a zero lease, or a lease ahead of the guest's epoch is a protocol violation. */
    if (serial == 0 || serial <= state->serial || lease == 0 || lease > state->epoch)
        return fail(state, GuestInputProtocolViolation);
    state->serial = serial;
    return lease < state->epoch ? GuestInputStaleEpoch : GuestInputOk;
}

guest_input_status_t guest_input_focus(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease, uint8_t pressed) {
    guest_input_status_t admitted;
    bool same;
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (window == 0 || generation == 0 || pressed > 1) return fail(state, GuestInputProtocolViolation);
    admitted = guest_input_admit_serial(state, serial, lease);
    if (admitted != GuestInputOk) return admitted;
    same = pair_matches(state, window, generation);
    if (pressed) {
        if (same) return GuestInputIgnored;
        if (state->pair_active && release_held(state) != GuestInputOk) return fail(state, GuestInputReleaseFailed);
        state->queue_count = 0;
        state->pair_active = true;
        state->pair_window = window;
        state->pair_generation = generation;
        state->lock_pending = true;
        return GuestInputOk;
    }
    if (!same) return GuestInputIgnored;
    if (release_held(state) != GuestInputOk) return fail(state, GuestInputReleaseFailed);
    state->pair_active = false;
    state->lock_pending = false;
    state->queue_count = 0;
    return GuestInputOk;
}

guest_input_status_t guest_input_key(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease, uint32_t key, uint8_t pressed) {
    guest_input_status_t admitted;
    guest_input_queued_t entry;
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (window == 0 || generation == 0 || key < 1 || key > GuestInputKeyMax || pressed > 1)
        return fail(state, GuestInputProtocolViolation);
    admitted = guest_input_admit_serial(state, serial, lease);
    if (admitted != GuestInputOk) return admitted;
    if (!pair_matches(state, window, generation)) return GuestInputIgnored;
    if (state->lock_pending) {
        entry.is_release = false;
        entry.pressed = pressed != 0;
        entry.key = (uint16_t)key;
        return enqueue(state, entry);
    }
    return apply_key(state, key, pressed != 0);
}

guest_input_status_t guest_input_release(guest_input_t *state, uint64_t window, uint64_t generation, uint32_t serial, uint64_t lease) {
    guest_input_status_t admitted;
    guest_input_queued_t entry;
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (window == 0 || generation == 0) return fail(state, GuestInputProtocolViolation);
    admitted = guest_input_admit_serial(state, serial, lease);
    if (admitted != GuestInputOk) return admitted;
    if (!pair_matches(state, window, generation)) return GuestInputIgnored;
    if (state->lock_pending) {
        entry.is_release = true;
        entry.pressed = false;
        entry.key = 0;
        return enqueue(state, entry);
    }
    if (release_held(state) != GuestInputOk) return fail(state, GuestInputReleaseFailed);
    return GuestInputOk;
}

guest_input_status_t guest_input_lock_state(guest_input_t *state, uint32_t serial, uint8_t locks, uint64_t lease) {
    guest_input_status_t admitted;
    waddle_keyboard_lock_update_t update;
    waddle_keyboard_lock_status_t planned;
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (locks > GuestInputLocksMax) return fail(state, GuestInputProtocolViolation);
    admitted = guest_input_admit_serial(state, serial, lease);
    if (admitted != GuestInputOk) return admitted;
    if (!state->pair_active) return GuestInputIgnored;
    planned = waddle_keyboard_lock_guest_update(&state->lock_guest, state->epoch, lease, serial, locks,
        current_lock_bits(state), &update);
    /* The executor already admitted this serial in this epoch, so the planner must accept it. */
    if (planned != WaddleKeyboardLockOk || !update.applied) return fail(state, GuestInputProtocolViolation);
    for (uint8_t index = 0; index < update.plan.count; index++) {
        const waddle_keyboard_lock_action_t action = update.plan.actions[index];
        if (inject_vk(state, action.vk, action.pressed) != 0) return fail(state, GuestInputInjectionFailed);
    }
    state->lock_pending = false;
    return drain_queue(state);
}

guest_input_status_t guest_input_repeat_info(guest_input_t *state, uint32_t interval_ms, uint32_t rate) {
    if (!state) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (interval_ms > GuestInputRepeatIntervalMax || rate > GuestInputRepeatRateMax) return GuestInputInvalidArgument;
    state->repeat_interval_ms = interval_ms;
    state->repeat_rate = rate;
    state->repeat_known = true;
    return GuestInputOk;
}

guest_input_status_t guest_input_keyboard_layout(guest_input_t *state, const uint8_t *klid, size_t length) {
    waddle_keyboard_layouts_klid_t parsed;
    wchar_t wide[GuestInputWideKlidChars];
    HWND window;
    HKL layout;
    size_t index;
    if (!state || !klid) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (waddle_keyboard_layouts_validate_klid(klid, length, &parsed) != WaddleKeyboardLayoutsOk)
        return GuestInputInvalidArgument;
    if (state->layout_known && memcmp(state->layout_klid, parsed.bytes, GuestInputKlidBytes) == 0)
        return GuestInputLayoutUnchanged;
    window = state->win32->get_foreground_window();
    if (!window || state->win32->get_window_thread_process_id(window, NULL) == 0) return GuestInputLayoutNoWindow;
    for (index = 0; index < GuestInputKlidBytes; index++) wide[index] = (wchar_t)parsed.bytes[index];
    wide[GuestInputKlidBytes] = L'\0';
    layout = state->win32->load_keyboard_layout(wide, KLF_ACTIVATE | KLF_SETFORPROCESS);
    if (!layout) return GuestInputLayoutLoadFailed;
    if (!state->win32->post_message(window, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)(uintptr_t)layout))
        return GuestInputLayoutPostFailed;
    memcpy(state->layout_klid, parsed.bytes, GuestInputKlidBytes);
    state->layout_known = true;
    return GuestInputOk;
}

guest_input_status_t guest_input_revoke(guest_input_t *state, uint64_t *next_epoch) {
    if (!state || !next_epoch) return GuestInputNullArgument;
    if (state->terminal) return GuestInputTerminal;
    if (release_held(state) != GuestInputOk) return fail(state, GuestInputReleaseFailed);
    if (state->epoch == UINT64_MAX) return fail(state, GuestInputEpochExhausted);
    state->pair_active = false;
    state->lock_pending = false;
    state->queue_count = 0;
    state->epoch++;
    *next_epoch = state->epoch;
    return GuestInputOk;
}

guest_input_status_t guest_input_release_all(guest_input_t *state) {
    if (!state) return GuestInputNullArgument;
    return release_held(state);
}

guest_input_status_t guest_input_disconnect(guest_input_t *state) {
    guest_input_status_t status;
    if (!state) return GuestInputNullArgument;
    status = release_held(state);
    state->terminal = true;
    state->pair_active = false;
    state->lock_pending = false;
    state->queue_count = 0;
    return status;
}

guest_input_status_t guest_input_translate_key(uint32_t key, guest_input_translation_t *out) {
    size_t index;
    if (!out) return GuestInputNullArgument;
    out->kind = GuestInputKeyUnsupported;
    out->scan_code = 0;
    out->extended = 0;
    out->virtual_key = 0;
    if (key >= 1 && key <= 83) {
        out->kind = GuestInputKeyScanCode;
        out->scan_code = (uint8_t)key;
        return GuestInputOk;
    }
    if (key > GuestInputKeyMax) return GuestInputOk;
    for (index = 0; index < sizeof GuestInputMappings / sizeof GuestInputMappings[0]; index++) {
        if (GuestInputMappings[index].key != key) continue;
        out->kind = GuestInputMappings[index].kind;
        out->scan_code = GuestInputMappings[index].scan_code;
        out->extended = GuestInputMappings[index].extended;
        out->virtual_key = GuestInputMappings[index].virtual_key;
        return GuestInputOk;
    }
    return GuestInputOk;
}

int guest_input_key_repeatable(uint32_t key) {
    size_t index;
    if (key == 0 || key > GuestInputKeyMax) return 0;
    for (index = 0; index < sizeof GuestInputNonRepeatKeys / sizeof GuestInputNonRepeatKeys[0]; index++) {
        if (GuestInputNonRepeatKeys[index] == key) return 0;
    }
    return 1;
}

#ifdef _WIN32

static UINT native_send_input(UINT count, LPINPUT inputs, int size) { return SendInput(count, inputs, size); }
static SHORT native_get_key_state(int virtual_key) { return GetKeyState(virtual_key); }
static HKL native_load_keyboard_layout(LPCWSTR klid, UINT flags) { return LoadKeyboardLayoutW(klid, flags); }
static BOOL native_post_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return PostMessageW(window, message, wparam, lparam);
}
static HWND native_get_foreground_window(void) { return GetForegroundWindow(); }
static DWORD native_get_window_thread_process_id(HWND window, LPDWORD process_id) {
    return GetWindowThreadProcessId(window, process_id);
}

static const guest_input_win32_t NativeWin32 = {
    native_send_input,
    native_get_key_state,
    native_load_keyboard_layout,
    native_post_message,
    native_get_foreground_window,
    native_get_window_thread_process_id,
};

const guest_input_win32_t *guest_input_win32_native(void) { return &NativeWin32; }

#endif
