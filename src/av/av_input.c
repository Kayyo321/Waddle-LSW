#include "av_input.h"

uint16_t av_input_scan_code(uint32_t key) {
    /* Standard PC evdev codes 1..83 match Set-1. Extended keys are explicit;
     * unsupported international/media keys are never guessed. */
    if (key >= 1 && key <= 83) return (uint16_t)key;
    switch (key) {
        case 86: return 0x56; /* ISO extra key. */
        case 87: return 0x57;
        case 88: return 0x58;
        case 96: return 0x11c;
        case 97: return 0x11d;
        case 98: return 0x135;
        case 99: return 0x137;
        case 100: return 0x138;
        case 102: return 0x147;
        case 103: return 0x148;
        case 104: return 0x149;
        case 105: return 0x14b;
        case 106: return 0x14d;
        case 107: return 0x14f;
        case 108: return 0x150;
        case 109: return 0x151;
        case 110: return 0x152;
        case 111: return 0x153;
        case 125: return 0x15b;
        case 126: return 0x15c;
        case 127: return 0x15d;
        default: return 0;
    }
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
    if (event->type == MsgInputFocus && event->flags) {
        int status = ops->target(context, event->window_id, event->sequence, -1);
        if (status > 0) return 0; /* Retired identity: ordered no-op. */
        if (status < 0) return -1;
        if (av_input_reset(state, ops, context) != 0 ||
            ops->target(context, event->window_id, event->sequence, 1) != 0) return -1;
        state->window_id = event->window_id;
        state->incarnation = event->sequence;
        return 0;
    }
    if (event->window_id != state->window_id || event->sequence != state->incarnation)
        return 0; /* Ordered stale focus/leave events cannot affect a new window. */
    if (event->type == MsgInputFocus) return av_input_reset(state, ops, context);
    if (event->type == MsgInputRelease) return release_held(state, event->flags, ops, context);
    if (ops->target(context, event->window_id, event->sequence, 0) != 0) return -1;
    if (event->type == MsgInputKey || event->type == MsgInputButton) {
        if (event->type == MsgInputKey && !av_input_scan_code(event->width)) return -1;
        uint8_t *held = event->type == MsgInputKey ? &state->keys[event->width] : &state->buttons[event->width];
        if (*held == event->flags) return 0;
        if (ops->emit(context, event) != 0) return -1;
        *held = (uint8_t)event->flags;
        return 0;
    }
    return ops->emit(context, event);
}
