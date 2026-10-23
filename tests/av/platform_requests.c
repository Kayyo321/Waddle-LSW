/** @file platform_requests.c @brief CPU regression for the exact native fixture callback. */
#include "platform_requests.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void rejected(av_platform_requests_t *requests, const av_message_t *message) {
    av_platform_requests_t before = *requests;
    assert(av_platform_record_request(message, requests) == -1);
    assert(memcmp(requests, &before, sizeof(before)) == 0);
}
int main(void) {
    av_platform_requests_t requests = {0};
    av_message_t message = {.type = MsgWindowGeometry, .window_id = AvPlatformWindow,
        .sequence = AvPlatformIncarnation, .width = 320, .height = 200, .dpi = 96};
    assert(av_platform_record_request(&message, &requests) == 0 && requests.controls == 1);
    message = (av_message_t){.type = MsgWindowClose, .window_id = AvPlatformWindow,
        .sequence = AvPlatformIncarnation};
    assert(av_platform_record_request(&message, &requests) == 0 && requests.controls == 2);
    /* A real compositor may focus the surface immediately after CreateV2. */
    for (uint32_t type = MsgInputFocus; type <= MsgInputRelease; ++type) {
        message = (av_message_t){.type = type, .window_id = AvPlatformWindow,
            .sequence = AvPlatformIncarnation, .buffer_index = type - MsgInputFocus + 1};
        if (type == MsgInputFocus || type == MsgInputKey || type == MsgInputButton || type == MsgInputRelease)
            message.flags = 1;
        if (type == MsgInputKey) message.width = 30;
        if (type == MsgInputButton) message.width = 1;
        if (type == MsgInputPointer) { message.x = 10; message.y = 20; }
        if (type == MsgInputWheel) message.y = 120;
        assert(av_platform_record_request(&message, &requests) == 0);
        assert(requests.inputs == type - MsgInputFocus + 1);
        rejected(&requests, &message); /* same input serial is a replay */
    }
    assert(requests.inputs == 6 && requests.controls == 2 && requests.last_input_serial == 6);
    message = (av_message_t){.type = MsgInputFocus, .window_id = AvPlatformWindow,
        .sequence = AvPlatformIncarnation, .buffer_index = 7, .flags = 0};
    assert(av_platform_record_request(&message, &requests) == 0); /* keyboard leave */
    message.buffer_index = 8; message.window_id = 0; rejected(&requests, &message);
    message.window_id = AvPlatformWindow + 1; rejected(&requests, &message);
    message.window_id = AvPlatformWindow; message.sequence = 0; rejected(&requests, &message);
    message.sequence = AvPlatformIncarnation + 1; rejected(&requests, &message);
    message.sequence = AvPlatformIncarnation; message.flags = 2; rejected(&requests, &message);
    message.flags = 1; message.buffer_index = 0; rejected(&requests, &message);
    message.buffer_index = 8; message.title[255] = 1; rejected(&requests, &message);
    for (uint32_t type = MsgWindowCreate; type <= MsgWindowCreateV2; ++type) {
        if (type == MsgWindowGeometry || type == MsgWindowClose ||
            (type >= MsgInputFocus && type <= MsgInputRelease)) continue;
        message = (av_message_t){.type = type, .window_id = AvPlatformWindow,
            .sequence = AvPlatformIncarnation, .width = 320, .height = 200,
            .dpi = 96, .process_id = 1, .damage_width = 320, .damage_height = 200};
        /* These are codec-valid but directionally unexpected fixture requests. */
        uint8_t bytes[AvControlBytes];
        assert(av_control_encode(&message, bytes, sizeof(bytes)) == 0);
        rejected(&requests, &message);
    }
    rejected(&requests, NULL);
    assert(av_platform_record_request(&message, NULL) == -1);
    puts("AV native-platform callback: controls, focus and six input kinds recorded; no guest injection");
    return 0;
}
