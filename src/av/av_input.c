#include "av_input.h"

typedef struct av_key_mapping_t {
    uint16_t key;
    av_key_translation_t translation;
} av_key_mapping_t;

/* This sorted immutable table is the frozen evdev-to-Windows keyboard map.
 * Values are typed so Pause, virtual keys, and suppressed keys can never be
 * mistaken for ordinary Set-1 scans by callers of the legacy scan ABI. */
static const av_key_mapping_t KeyMappings[] = {
    {85, {.kind = AvKeyTranslationScanCode, .scan_code = 0x29}},
    {86, {.kind = AvKeyTranslationScanCode, .scan_code = 0x56}},
    {87, {.kind = AvKeyTranslationScanCode, .scan_code = 0x57}},
    {88, {.kind = AvKeyTranslationScanCode, .scan_code = 0x58}},
    {89, {.kind = AvKeyTranslationScanCode, .scan_code = 0x73}},
    {90, {.kind = AvKeyTranslationScanCode, .scan_code = 0x70}},
    {92, {.kind = AvKeyTranslationScanCode, .scan_code = 0x79}},
    {93, {.kind = AvKeyTranslationScanCode, .scan_code = 0x70}},
    {94, {.kind = AvKeyTranslationScanCode, .scan_code = 0x7b}},
    {95, {.kind = AvKeyTranslationScanCode, .scan_code = 0x5c}},
    {96, {.kind = AvKeyTranslationScanCode, .scan_code = 0x1c, .extended = 1}},
    {97, {.kind = AvKeyTranslationScanCode, .scan_code = 0x1d, .extended = 1}},
    {98, {.kind = AvKeyTranslationScanCode, .scan_code = 0x35, .extended = 1}},
    {99, {.kind = AvKeyTranslationScanCode, .scan_code = 0x37, .extended = 1}},
    {100, {.kind = AvKeyTranslationScanCode, .scan_code = 0x38, .extended = 1}},
    {102, {.kind = AvKeyTranslationScanCode, .scan_code = 0x47, .extended = 1}},
    {103, {.kind = AvKeyTranslationScanCode, .scan_code = 0x48, .extended = 1}},
    {104, {.kind = AvKeyTranslationScanCode, .scan_code = 0x49, .extended = 1}},
    {105, {.kind = AvKeyTranslationScanCode, .scan_code = 0x4b, .extended = 1}},
    {106, {.kind = AvKeyTranslationScanCode, .scan_code = 0x4d, .extended = 1}},
    {107, {.kind = AvKeyTranslationScanCode, .scan_code = 0x4f, .extended = 1}},
    {108, {.kind = AvKeyTranslationScanCode, .scan_code = 0x50, .extended = 1}},
    {109, {.kind = AvKeyTranslationScanCode, .scan_code = 0x51, .extended = 1}},
    {110, {.kind = AvKeyTranslationScanCode, .scan_code = 0x52, .extended = 1}},
    {111, {.kind = AvKeyTranslationScanCode, .scan_code = 0x53, .extended = 1}},
    {113, {.kind = AvKeyTranslationScanCode, .scan_code = 0x20, .extended = 1}},
    {114, {.kind = AvKeyTranslationScanCode, .scan_code = 0x2e, .extended = 1}},
    {115, {.kind = AvKeyTranslationScanCode, .scan_code = 0x30, .extended = 1}},
    {116, {.kind = AvKeyTranslationSuppressed}},
    {117, {.kind = AvKeyTranslationScanCode, .scan_code = 0x59}},
    {119, {.kind = AvKeyTranslationPause, .pause_make = {0xe1, 0x1d, 0x45},
           .pause_break = {0xe1, 0x9d, 0xc5}}},
    {121, {.kind = AvKeyTranslationScanCode, .scan_code = 0x7e}},
    {122, {.kind = AvKeyTranslationVirtualKey, .virtual_key = 0x15}}, /* VK_HANGUL. */
    {123, {.kind = AvKeyTranslationVirtualKey, .virtual_key = 0x19}}, /* VK_HANJA. */
    {124, {.kind = AvKeyTranslationScanCode, .scan_code = 0x7d}},
    {125, {.kind = AvKeyTranslationScanCode, .scan_code = 0x5b, .extended = 1}},
    {126, {.kind = AvKeyTranslationScanCode, .scan_code = 0x5c, .extended = 1}},
    {127, {.kind = AvKeyTranslationScanCode, .scan_code = 0x5d, .extended = 1}},
    {128, {.kind = AvKeyTranslationScanCode, .scan_code = 0x68, .extended = 1}},
    {140, {.kind = AvKeyTranslationScanCode, .scan_code = 0x21, .extended = 1}},
    {142, {.kind = AvKeyTranslationSuppressed}},
    {150, {.kind = AvKeyTranslationScanCode, .scan_code = 0x32, .extended = 1}},
    {155, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6c, .extended = 1}},
    {156, {.kind = AvKeyTranslationScanCode, .scan_code = 0x66, .extended = 1}},
    {158, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6a, .extended = 1}},
    {159, {.kind = AvKeyTranslationScanCode, .scan_code = 0x69, .extended = 1}},
    {163, {.kind = AvKeyTranslationScanCode, .scan_code = 0x19, .extended = 1}},
    {164, {.kind = AvKeyTranslationScanCode, .scan_code = 0x22, .extended = 1}},
    {165, {.kind = AvKeyTranslationScanCode, .scan_code = 0x10, .extended = 1}},
    {166, {.kind = AvKeyTranslationScanCode, .scan_code = 0x24, .extended = 1}},
    {172, {.kind = AvKeyTranslationScanCode, .scan_code = 0x32, .extended = 1}},
    {173, {.kind = AvKeyTranslationScanCode, .scan_code = 0x67, .extended = 1}},
    {183, {.kind = AvKeyTranslationScanCode, .scan_code = 0x64}},
    {184, {.kind = AvKeyTranslationScanCode, .scan_code = 0x65}},
    {185, {.kind = AvKeyTranslationScanCode, .scan_code = 0x66}},
    {186, {.kind = AvKeyTranslationScanCode, .scan_code = 0x67}},
    {187, {.kind = AvKeyTranslationScanCode, .scan_code = 0x68}},
    {188, {.kind = AvKeyTranslationScanCode, .scan_code = 0x69}},
    {189, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6a}},
    {190, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6b}},
    {191, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6c}},
    {192, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6d}},
    {193, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6e}},
    {194, {.kind = AvKeyTranslationScanCode, .scan_code = 0x76}},
    {217, {.kind = AvKeyTranslationScanCode, .scan_code = 0x65, .extended = 1}},
    {226, {.kind = AvKeyTranslationScanCode, .scan_code = 0x6d, .extended = 1}},
};

av_key_translation_t av_input_translate_key(uint32_t key) {
    if (key >= 1 && key <= 83)
        return (av_key_translation_t){.kind = AvKeyTranslationScanCode, .scan_code = (uint8_t)key};
    if (key > 767) return (av_key_translation_t){.kind = AvKeyTranslationUnsupported};
    for (size_t index = 0; index < sizeof(KeyMappings) / sizeof(KeyMappings[0]); ++index)
        if (KeyMappings[index].key == key) return KeyMappings[index].translation;
    return (av_key_translation_t){.kind = AvKeyTranslationUnsupported};
}

uint16_t av_input_scan_code(uint32_t key) {
    /* Legacy callers and the frozen AV wire codec admit only 1..127. */
    if (key == 0 || key >= AvInputKeys) return 0;
    const av_key_translation_t translation = av_input_translate_key(key);
    if (translation.kind != AvKeyTranslationScanCode) return 0;
    return (uint16_t)translation.scan_code | (translation.extended ? 0x100 : 0);
}
/* Keys that never auto-repeat: modifiers, lock keys, Pause, and the suppressed
 * power/sleep keys. Same set as keyboard_repeat.zig policy_repeatable(). */
static const uint16_t NonRepeatKeys[] = {29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 125, 126, 142};

int av_input_key_repeatable(uint32_t key) {
    if (key == 0 || key > 767) return 0;
    for (size_t index = 0; index < sizeof(NonRepeatKeys) / sizeof(NonRepeatKeys[0]); ++index)
        if (NonRepeatKeys[index] == key) return 0;
    return 1;
}

static int release_held(av_input_state_t *state, unsigned mask,
                        const av_input_ops_t *ops, void *context) {
    int result = 0;
    av_message_t event = {.window_id = state->window_id, .sequence = state->incarnation};
    if (mask & AvInputReleaseKeys) {
        event.type = MsgInputKey;
        for (unsigned key = 1; key < AvInputKeys; ++key) {
            if (!state->keys[key]) continue;
            event.width = key;
            if (ops->emit(context, &event) != 0) result = -1;
            else state->keys[key] = 0;
        }
    }
    if (mask & AvInputReleaseButtons) {
        event.type = MsgInputButton;
        for (unsigned button = 1; button <= 3; ++button) {
            if (!state->buttons[button]) continue;
            event.width = button;
            if (ops->emit(context, &event) != 0) result = -1;
            else state->buttons[button] = 0;
        }
    }
    return result;
}
int av_input_reset(av_input_state_t *state, const av_input_ops_t *ops, void *context) {
    if (!state || !ops || !ops->emit) return -1;
    if (release_held(state, AvInputReleaseKeys | AvInputReleaseButtons, ops, context) != 0)
        return -1;
    state->window_id = 0;
    state->incarnation = 0;
    return 0;
}
int av_input_apply(av_input_state_t *state, const av_message_t *event,
                   const av_input_ops_t *ops, void *context) {
    uint8_t bytes[AvControlBytes];
    if (!state || !event || !ops || !ops->target || !ops->emit ||
        event->type < MsgInputFocus || event->type > MsgInputRelease ||
        av_control_encode(event, bytes, sizeof(bytes)) != 0 || event->buffer_index <= state->serial)
        return -1;
    state->serial = event->buffer_index;
    return av_input_apply_admitted(state, event, ops, context) == 0 ? 0 : -1;
}
int av_input_apply_admitted(av_input_state_t *state, const av_message_t *event,
                            const av_input_ops_t *ops, void *context) {
    if (!state || !event || !ops || !ops->target || !ops->emit ||
        event->type < MsgInputFocus || event->type > MsgInputRelease) return -1;
    if (event->type == MsgInputKey &&
        (event->width == 0 || event->width >= AvInputKeys || !av_input_scan_code(event->width))) return -1;
    if (event->type == MsgInputButton && (event->width == 0 || event->width >= 4)) return -1;
    if (event->type == MsgInputFocus && event->flags) {
        int status = ops->target(context, event->window_id, event->sequence, -1);
        if (status > 0) return 0; /* Retired identity: ordered no-op. */
        if (status < 0) return -1;
        if (av_input_reset(state, ops, context) != 0) return -1;
        int activated = ops->target(context, event->window_id, event->sequence, 1);
        if (activated != 0) return activated > 0 ? 1 : -1;
        state->window_id = event->window_id;
        state->incarnation = event->sequence;
        return 0;
    }
    if (event->window_id != state->window_id || event->sequence != state->incarnation)
        return 0; /* Ordered stale focus/leave events cannot affect a new window. */
    if (event->type == MsgInputFocus) return av_input_reset(state, ops, context);
    if (event->type == MsgInputRelease) return release_held(state, event->flags, ops, context);
    int target = ops->target(context, event->window_id, event->sequence, 0);
    if (target != 0) return target > 0 ? 1 : -1;
    if (event->type == MsgInputKey || event->type == MsgInputButton) {
        uint8_t *held = event->type == MsgInputKey ? &state->keys[event->width] : &state->buttons[event->width];
        /* A repeated down for a held repeatable key is an admitted typematic repeat:
         * it is injected as a plain down with no intervening up (IMPL_DESC 3.6.3). */
        int repeat = event->type == MsgInputKey && event->flags && *held &&
            av_input_key_repeatable(event->width);
        if (*held == event->flags && !repeat) return 0;
        int emitted = ops->emit(context, event);
        if (emitted != 0) return emitted > 0 ? 1 : -1;
        *held = (uint8_t)event->flags;
        return 0;
    }
    return ops->emit(context, event);
}
