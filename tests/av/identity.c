/** @file identity.c @brief Portable admission/effect gate rejects stale native work. */
#include "av_identity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned effects;
static int effect_result;
static int native_effect(const av_message_t *message, void *context) {
    assert(message && context == &effects);
    ++effects;
    return effect_result;
}
int main(void) {
    av_message_t first = {.type = MsgWindowCreateV2, .window_id = 42, .sequence = 10,
        .width = 640, .height = 480, .dpi = 96, .process_id = 100};
    av_identity_session_t session = {0};
    assert(av_identity_admit(NULL, &first) == -1);
    assert(av_identity_admit(&session, NULL) == -1);
    av_message_t invalid = first; invalid.type = MsgWindowClose;
    assert(av_identity_admit(&session, &invalid) == -1);
    invalid = first; invalid.sequence = 0;
    assert(av_identity_admit(&session, &invalid) == -1 && session.mode == AvIdentityUnknown);
    assert(av_identity_admit(&session, &first) == 0 && session.mode == AvIdentityModern);
    av_identity_session_t sentinel = session;
    assert(av_identity_admit(&session, &first) == -1);
    invalid = first; invalid.sequence = 9;
    assert(av_identity_admit(&session, &invalid) == -1);
    assert(memcmp(&session, &sentinel, sizeof(session)) == 0);
    av_message_t second = first; second.window_id = 43; second.sequence = 20;
    assert(av_identity_admit(&session, &second) == 0);
    av_message_t control = first; control.type = MsgWindowGeometry;
    assert(av_identity_match(&first, &control) == 1); /* high-water is not a control floor */
    assert(av_identity_apply(&first, &control, native_effect, &effects) == 0 && effects == 1);
    control.type = MsgWindowDestroy;
    assert(av_identity_match(&first, &control) == 1);
    assert(av_identity_apply(&first, &control, native_effect, &effects) == -1 && effects == 1);
    second.window_id = first.window_id; /* A retired; B is admitted with the same HWND. */
    for (unsigned flags = 0; flags <= 3; ++flags) {
        control = first; control.type = MsgWindowGeometry; control.flags = flags;
        assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 1);
        control.type = MsgWindowClose;
        assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 1);
        control.type = MsgWindowDestroy;
        assert(av_identity_match(&second, &control) == 0 && effects == 1);
    }
    control = second; control.type = MsgWindowGeometry;
    assert(av_identity_apply(NULL, &control, native_effect, &effects) == 0 && effects == 1);
    control.window_id = 99;
    assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 1);
    control = second; control.type = MsgWindowGeometry; control.sequence = 30;
    assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 1);
    control.sequence = second.sequence; control.process_id = 101;
    assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 1);
    control.process_id = 0;
    for (unsigned flags = 0; flags <= 3; ++flags) {
        control.flags = flags;
        assert(av_identity_apply(&second, &control, native_effect, &effects) == 0);
    }
    assert(effects == 5);
    control.type = MsgWindowClose;
    assert(av_identity_apply(&second, &control, native_effect, &effects) == 0 && effects == 6);
    effect_result = -1;
    assert(av_identity_apply(&second, &control, native_effect, &effects) == -1 && effects == 7);
    effect_result = 0;
    control.sequence = 0;
    assert(av_identity_apply(NULL, &control, native_effect, &effects) == -1 && effects == 7);
    assert(av_identity_apply(&second, &control, native_effect, &effects) == -1 && effects == 7);
    control.sequence = second.sequence; control.window_id = 0;
    assert(av_identity_match(&second, &control) == -1);
    assert(av_identity_match(&second, NULL) == -1);
    control = second; control.type = MsgFrameReady;
    assert(av_identity_match(&second, &control) == -1);
    assert(av_identity_apply(&second, NULL, native_effect, &effects) == -1);
    assert(av_identity_apply(&second, &control, NULL, &effects) == -1);
    control.type = MsgWindowClose; second.sequence = 0;
    assert(av_identity_match(&second, &control) == 0);
    invalid = first; invalid.type = MsgWindowCreate;
    assert(av_identity_admit(&session, &invalid) == -1);
    session = (av_identity_session_t){0}; invalid.sequence = 0;
    assert(av_identity_admit(&session, &invalid) == 0 && session.mode == AvIdentityLegacy);
    invalid.sequence = 100;
    assert(av_identity_admit(&session, &invalid) == 0 && session.mode == AvIdentityLegacy && !session.high_water);
    assert(av_identity_admit(&session, &first) == -1);
    session = (av_identity_session_t){0}; first.sequence = UINT64_MAX;
    assert(av_identity_admit(&session, &first) == 0);
    assert(av_identity_admit(&session, &first) == -1);
    uint64_t counter = UINT64_MAX - 1, token = 99;
    assert(av_identity_next(&counter, &token) == 0 && token == UINT64_MAX);
    token = 99;
    assert(av_identity_next(&counter, &token) == -1 && counter == UINT64_MAX && token == 99);
    counter = 0; session = (av_identity_session_t){0};
    assert(av_identity_next(&counter, &token) == 0 && token == 1);
    first.sequence = token;
    assert(av_identity_admit(&session, &first) == 0); /* reconnect is connection-local */
    puts("AV identity: capability lock, high-water, stale zero-effect controls and exhaustion passed");
    return 0;
}
