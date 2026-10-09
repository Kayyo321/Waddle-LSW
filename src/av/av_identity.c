#include "av_identity.h"
int av_identity_admit(av_identity_session_t *session, const av_message_t *message) {
    uint8_t check[AvControlBytes];
    if (!session || !message || (message->type != MsgWindowCreate && message->type != MsgWindowCreateV2) ||
        av_control_encode(message, check, sizeof(check)) != 0) return -1;
    av_identity_mode_t mode = message->type == MsgWindowCreateV2 ? AvIdentityModern : AvIdentityLegacy;
    if (session->mode != AvIdentityUnknown && session->mode != mode) return -1;
    if (mode == AvIdentityModern && message->sequence <= session->high_water) return -1;
    session->mode = mode;
    if (mode == AvIdentityModern) session->high_water = message->sequence;
    return 0;
}
int av_identity_next(uint64_t *counter, uint64_t *token) {
    if (*counter == UINT64_MAX) return -1;
    *token = ++*counter;
    return 0;
}
int av_identity_match(const av_message_t *current, const av_message_t *message) {
    uint8_t check[AvControlBytes];
    if (!message || (message->type != MsgWindowGeometry && message->type != MsgWindowDestroy &&
        message->type != MsgWindowClose) || !message->sequence ||
        av_control_encode(message, check, sizeof(check)) != 0) return -1;
    return current && current->sequence && current->window_id == message->window_id &&
        current->sequence == message->sequence &&
        (!message->process_id || current->process_id == message->process_id);
}
int av_identity_apply(const av_message_t *current, const av_message_t *message,
                      av_identity_effect_t effect, void *context) {
    if (!message || !effect || (message->type != MsgWindowGeometry && message->type != MsgWindowClose))
        return -1;
    int match = av_identity_match(current, message);
    return match == 1 ? effect(message, context) : match;
}
