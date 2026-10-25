/**
 * @file session_input_handler_test.c
 * @brief Handler-level tests for the host keyboard feature: frames, lease barrier, queue, timing.
 * @note Built with -Wl,--wrap for interop_host_feature_send (records frames) and clock_gettime
 *       (deterministic monotonic time). Drives the real session_handler_t callbacks; the full
 *       control runtime is not started (see the contract for that limit).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session_input_handler.h"
#include "waddle/interop_protocol.h"

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

enum { SentCapacity = 8192, FrameCapacity = 160 };

typedef struct sent_frame_t {
    uint16_t type;
    size_t length;
    uint8_t bytes[FrameCapacity];
} sent_frame_t;

static sent_frame_t g_sent[SentCapacity];
static size_t g_sent_count;
static int g_send_result = HostFeatureSuccess;
static uint64_t g_now_ns = 5000000000ull;
static char g_sender_token;
static interop_host_feature_sender_t *const g_sender = (interop_host_feature_sender_t *)&g_sender_token;
static size_t g_lock_calls;
static int g_inhibit_actions;
static int g_inhibit_last_kind;
static uint64_t g_inhibit_last_window;
static int g_dispatch_calls;
static int g_dispatch_result;

/** @brief Fake transport: records successful frames; returns g_send_result otherwise. */
int __wrap_interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length) {
    (void)sender;
    if (g_send_result != HostFeatureSuccess) return g_send_result;
    CHECK(length <= FrameCapacity);
    CHECK(g_sent_count < SentCapacity);
    interop_header_t header;
    CHECK(interop_header_decode(frame, length, &header) == InteropErrorNone);
    sent_frame_t *slot = &g_sent[g_sent_count++];
    slot->type = header.type;
    slot->length = length;
    memcpy(slot->bytes, frame, length);
    return HostFeatureSuccess;
}

static int g_clock_fail;
static int g_clock_bad;

/** @brief Deterministic monotonic clock; g_clock_fail forces an error, g_clock_bad an invalid nanosecond field. */
int __wrap_clock_gettime(clockid_t clock, struct timespec *ts) {
    (void)clock;
    if (g_clock_fail) return -1;
    if (g_clock_bad) {
        ts->tv_sec = 1;
        ts->tv_nsec = 2000000000L;
        return 0;
    }
    ts->tv_sec = (time_t)(g_now_ns / 1000000000ull);
    ts->tv_nsec = (long)(g_now_ns % 1000000000ull);
    return 0;
}

static void advance_ms(uint64_t ms) { g_now_ns += ms * 1000000ull; }

static void reset_sent(void) { g_sent_count = 0; }

static uint64_t field_of(const sent_frame_t *frame, uint16_t tag, uint32_t width) {
    uint64_t value = 0;
    CHECK(interop_field_uint(frame->bytes + InteropHeaderBytes, frame->length - InteropHeaderBytes,
              tag, width, &value) == InteropErrorNone);
    return value;
}

static const sent_frame_t *expect_one(uint16_t type) {
    CHECK(g_sent_count >= 1);
    CHECK(g_sent[0].type == type);
    return &g_sent[0];
}

/** @brief Builds a guest-direction lease frame. has_serial selects a Serial field. */
static size_t build_guest_lease(uint8_t *out, uint16_t kind, int has_serial, uint32_t serial, uint64_t epoch) {
    interop_writer_t writer;
    interop_writer_init(&writer, out, FrameCapacity);
    if (has_serial) CHECK(interop_put_uint(&writer, InteropTagSerial, serial) == InteropErrorNone);
    CHECK(interop_put_uint(&writer, InteropTagLeaseEpoch, epoch) == InteropErrorNone);
    size_t length = 0;
    CHECK(interop_writer_finish(&writer, kind, 0u, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

/** @brief Builds a guest CaretRect frame, which the handler owns and ignores. */
static size_t build_caret(uint8_t *out) {
    interop_writer_t writer;
    interop_writer_init(&writer, out, FrameCapacity);
    CHECK(interop_put_uint(&writer, InteropTagWindow, 0x1001u) == InteropErrorNone);
    CHECK(interop_put_uint(&writer, InteropTagGeneration, 7u) == InteropErrorNone);
    CHECK(interop_put_rect(&writer, InteropTagRect, 1, 2, 3, 4) == InteropErrorNone);
    size_t length = 0;
    CHECK(interop_writer_finish(&writer, InteropMsgCaretRect, 0u, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

static int deliver(session_handler_t *handler, uint16_t type, const uint8_t *frame, size_t length) {
    return handler->on_frame(handler->context, type, 0u, frame, length, g_sender);
}

static int deliver_lease(session_handler_t *handler, uint16_t kind, int has_serial, uint32_t serial, uint64_t epoch) {
    uint8_t frame[FrameCapacity];
    size_t length = build_guest_lease(frame, kind, has_serial, serial, epoch);
    return deliver(handler, kind, frame, length);
}

static int tick(session_handler_t *handler) {
    return handler->on_tick(handler->context, 0u, g_sender);
}

/** @brief Records inhibit actions so the Wayland glue contract can be checked in-process. */
static void inhibit_recorder(void *context, const waddle_keyboard_inhibit_result_t *result) {
    (void)context;
    for (uint8_t index = 0; index < result->count; index++) {
        g_inhibit_actions++;
        g_inhibit_last_kind = result->actions[index].kind;
        g_inhibit_last_window = result->actions[index].window;
    }
}

/** @brief Fresh state with the handler ready for the lease handshake; no frames sent yet. */
static void begin(session_input_t *input, session_handler_t *handler, uint8_t mode) {
    reset_sent();
    g_send_result = HostFeatureSuccess;
    CHECK(session_input_init(input, mode, inhibit_recorder, NULL) == SessionInputPostOk);
    *handler = session_input_make_handler(input);
    CHECK(handler->on_ready(handler->context, InteropCapInput | InteropCapKeyboardLayout, g_sender) == 0);
}

/** @brief Runs Revoked(epoch) then Ready for the Ack serial; returns the Ack serial. */
static uint32_t handshake(session_handler_t *handler, uint64_t epoch) {
    reset_sent();
    CHECK(deliver_lease(handler, InteropMsgLeaseRevoked, 0, 0u, epoch) == 0);
    const sent_frame_t *ack = expect_one(InteropMsgLeaseAck);
    uint32_t serial = (uint32_t)field_of(ack, InteropTagSerial, 4u);
    CHECK(field_of(ack, InteropTagLeaseEpoch, 8u) == epoch);
    CHECK(deliver_lease(handler, InteropMsgLeaseReady, 1, serial, epoch) == 0);
    reset_sent();
    return serial;
}

/** @brief Converts the key table order to guest-facing expectations for a keyboard layout. */
static void expected_klid(const char *layout, const char *variant, uint8_t out[WaddleKeyboardLayoutsKlidBytes], int *kind) {
    waddle_keyboard_layouts_state_t state;
    memset(&state, 0, sizeof state);
    waddle_keyboard_layouts_outcome_t outcome;
    memset(&outcome, 0, sizeof outcome);
    CHECK(waddle_keyboard_layouts_observe(&state, (const uint8_t *)layout, strlen(layout),
              (const uint8_t *)variant, variant == NULL ? 0u : strlen(variant), &outcome) == WaddleKeyboardLayoutsOk);
    memcpy(out, outcome.klid.bytes, WaddleKeyboardLayoutsKlidBytes);
    *kind = outcome.kind;
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                    */
/* ---------------------------------------------------------------------- */

static void test_descriptor_and_init(void) {
    session_input_t input;
    CHECK(session_input_init(NULL, 0, NULL, NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_init(&input, 3, NULL, NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_init(&input, WaddleKeyboardInhibitFocused, NULL, NULL) == SessionInputPostOk);
    session_handler_t handler = session_input_make_handler(&input);
    CHECK(strcmp(handler.name, "session_input") == 0);
    CHECK(handler.capabilities == (InteropCapInput | InteropCapKeyboardLayout));
    CHECK(handler.owns_type(handler.context, InteropMsgLeaseRevoked));
    CHECK(handler.owns_type(handler.context, InteropMsgLeaseReady));
    CHECK(handler.owns_type(handler.context, InteropMsgCaretRect));
    CHECK(!handler.owns_type(handler.context, InteropMsgInputKey));
    CHECK(session_input_set_display(&input, -1, NULL, NULL) == SessionInputPostOk);
    CHECK(session_input_set_display(&input, 4, NULL, NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_set_display(NULL, -1, NULL, NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_key(NULL, 30, 1) == SessionInputPostInvalidArgument);
    CHECK(session_input_init(&input, WaddleKeyboardInhibitNever, NULL, NULL) == SessionInputPostOk);
    CHECK(input.inhibit.config == WaddleKeyboardInhibitNever);
}

static void test_handshake_and_forwarding(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);

    /* Enter before any lease is not forwarded and never replayed. */
    CHECK(session_input_post_focus_enter(&input, 0x1001u, 7u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 0);

    /* Handshake: Revoked(1) then Ack serial 1, then Ready. */
    reset_sent();
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    const sent_frame_t *ack = expect_one(InteropMsgLeaseAck);
    CHECK(field_of(ack, InteropTagSerial, 4u) == 1u);
    CHECK(field_of(ack, InteropTagLeaseEpoch, 8u) == 1u);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) == 0);
    CHECK(input.phase == SessionInputReady);
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);

    /* Physical focus was already on the window, so a fresh enter is required to forward. */
    reset_sent();
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x1001u, 7u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    const sent_frame_t *focus = expect_one(InteropMsgInputFocus);
    CHECK(field_of(focus, InteropTagWindow, 8u) == 0x1001u);
    CHECK(field_of(focus, InteropTagGeneration, 8u) == 7u);
    CHECK(field_of(focus, InteropTagSerial, 4u) == 2u);
    CHECK(field_of(focus, InteropTagPressed, 1u) == 1u);
    CHECK(field_of(focus, InteropTagLeaseEpoch, 8u) == 1u);

    /* Key down, stray up, duplicate down, up. Serials are 3 and 4. */
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(session_input_post_key(&input, 31u, 0u) == SessionInputPostOk);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(session_input_post_key(&input, 30u, 0u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 2u);
    CHECK(g_sent[0].type == InteropMsgInputKey && g_sent[1].type == InteropMsgInputKey);
    CHECK(field_of(&g_sent[0], InteropTagSerial, 4u) == 3u);
    CHECK(field_of(&g_sent[0], InteropTagKey, 2u) == 30u);
    CHECK(field_of(&g_sent[0], InteropTagPressed, 1u) == 1u);
    CHECK(field_of(&g_sent[1], InteropTagSerial, 4u) == 4u);
    CHECK(field_of(&g_sent[1], InteropTagPressed, 1u) == 0u);

    /* Invalid codes are rejected and never sent. */
    CHECK(session_input_post_key(&input, 0u, 1u) == SessionInputPostInvalidKey);
    CHECK(session_input_post_key(&input, 768u, 1u) == SessionInputPostInvalidKey);
    CHECK(session_input_post_key(&input, 767u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(input.held_count == 1u);
    CHECK(session_input_post_key(&input, 767u, 0u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);

    /* CaretRect is owned and ignored. */
    uint8_t caret[FrameCapacity];
    size_t caret_length = build_caret(caret);
    reset_sent();
    CHECK(deliver(&handler, InteropMsgCaretRect, caret, caret_length) == 0);
    CHECK(g_sent_count == 0);

    session_input_post_focus_leave(&input);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_release_on_leave_and_lock(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x2002u, 9u) == SessionInputPostOk);

    /* Lock state is sent once per enter, after the first modifiers observation. */
    CHECK(session_input_post_modifiers(&input, 0u, 0u, 0x12u) == SessionInputPostOk);
    CHECK(session_input_post_modifiers(&input, 0u, 0u, 0x12u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 2u);
    CHECK(g_sent[0].type == InteropMsgInputFocus);
    CHECK(g_sent[1].type == InteropMsgInputLockState);
    CHECK(field_of(&g_sent[1], InteropTagSerial, 4u) == 3u);
    CHECK(field_of(&g_sent[1], InteropTagLocks, 1u) == 0x03u);
    CHECK(field_of(&g_sent[1], InteropTagLeaseEpoch, 8u) == 1u);

    /* A key held when focus leaves is released before the focus-off, both for this pair. */
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 3u);
    CHECK(g_sent[0].type == InteropMsgInputKey);
    CHECK(g_sent[1].type == InteropMsgInputRelease);
    CHECK(field_of(&g_sent[1], InteropTagWindow, 8u) == 0x2002u);
    CHECK(field_of(&g_sent[1], InteropTagGeneration, 8u) == 9u);
    CHECK(g_sent[2].type == InteropMsgInputFocus);
    CHECK(field_of(&g_sent[2], InteropTagPressed, 1u) == 0u);
    CHECK(input.held_count == 0u);

    /* Leave without a held key sends only the focus-off. Leave with no focus is a no-op. */
    reset_sent();
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 0u);

    /* Switching pairs while focused releases the old pair first. */
    CHECK(session_input_post_focus_enter(&input, 0x2002u, 9u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    reset_sent();
    CHECK(session_input_post_key(&input, 31u, 1u) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x3003u, 4u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent[0].type == InteropMsgInputKey);
    CHECK(g_sent[1].type == InteropMsgInputRelease);
    CHECK(g_sent[2].type == InteropMsgInputFocus && field_of(&g_sent[2], InteropTagPressed, 1u) == 0u);
    CHECK(g_sent[3].type == InteropMsgInputFocus && field_of(&g_sent[3], InteropTagPressed, 1u) == 1u);
    CHECK(field_of(&g_sent[3], InteropTagWindow, 8u) == 0x3003u);
    session_input_post_focus_leave(&input);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_layout_repeat_and_keymap(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    uint8_t klid[WaddleKeyboardLayoutsKlidBytes];
    int kind = -1;
    expected_klid("de", NULL, klid, &kind);
    CHECK(kind == WaddleKeyboardLayoutsSend);

    /* Repeat info and layout are not serial-bearing and go out without a lease. */
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_repeat_info(&input, -1, 0) == SessionInputPostInvalidArgument);
    const char *keymap = "xkb_symbols \"x\" {\n    include \"pc+de+inet(evdev)\"\n};\n";
    CHECK(session_input_post_keymap(&input, (const uint8_t *)keymap, strlen(keymap)) == SessionInputPostOk);
    CHECK(session_input_post_keymap(&input, NULL, 3u) == SessionInputPostInvalidArgument);
    reset_sent();
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 2u);
    CHECK(g_sent[0].type == InteropMsgKeyboardLayout);
    CHECK(g_sent[1].type == InteropMsgInputRepeatInfo);
    /* A reconnect (on_ready) re-sends the last known layout and repeat values. */
    reset_sent();
    CHECK(handler.on_ready(handler.context, InteropCapInput | InteropCapKeyboardLayout, g_sender) == 0);
    CHECK(g_sent_count == 2u);
    CHECK(g_sent[0].type == InteropMsgKeyboardLayout);
    CHECK(g_sent[1].type == InteropMsgInputRepeatInfo);
    CHECK(field_of(&g_sent[1], InteropTagIntervalMs, 4u) == 500u);
    CHECK(field_of(&g_sent[1], InteropTagRate, 4u) == 10u);
    const uint8_t *layout_value = NULL;
    uint16_t layout_length = 0;
    CHECK(interop_field_get(g_sent[0].bytes + InteropHeaderBytes, g_sent[0].length - InteropHeaderBytes,
              InteropTagLayout, 0u, &layout_value, &layout_length) == InteropErrorNone);
    CHECK(layout_length == WaddleKeyboardLayoutsKlidBytes);
    CHECK(memcmp(layout_value, klid, WaddleKeyboardLayoutsKlidBytes) == 0);

    /* A keymap that the guest cannot map changes nothing; a rejected keymap stops forwarding. */
    const char *invalid_keymap = "xkb_symbols \"x\" {\n    include \"pc+US\"\n};\n";
    const char *unmapped = "xkb_symbols \"x\" {\n    include \"pc+zz+inet(evdev)\"\n};\n";
    CHECK(session_input_post_keymap(&input, (const uint8_t *)unmapped, strlen(unmapped)) == SessionInputPostOk);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)invalid_keymap, strlen(invalid_keymap)) == SessionInputPostKeymapInvalid);
    /* Rejected keymap while not forwarding: no focus-off, status only. */
    handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x4004u, 2u) == SessionInputPostOk);
    CHECK(session_input_post_key(&input, 20u, 1u) == SessionInputPostOk);
    reset_sent();
    CHECK(session_input_post_keymap(&input, (const uint8_t *)invalid_keymap, strlen(invalid_keymap)) == SessionInputPostKeymapInvalid);
    CHECK(tick(&handler) == 0);
    /* Queued in order: focus-on, the held key down, then release-all and focus-off for the pair. */
    CHECK(g_sent_count == 4u);
    CHECK(g_sent[0].type == InteropMsgInputFocus && field_of(&g_sent[0], InteropTagPressed, 1u) == 1u);
    CHECK(g_sent[1].type == InteropMsgInputKey);
    CHECK(g_sent[2].type == InteropMsgInputRelease);
    CHECK(g_sent[3].type == InteropMsgInputFocus && field_of(&g_sent[3], InteropTagPressed, 1u) == 0u);
    CHECK(!input.forwarding);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_repeat_timing(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x5005u, 3u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);

    /* The first repeat is due after the configured delay; next_deadline reports it in whole ms. */
    uint64_t deadline = handler.next_deadline_ms(handler.context, 0u);
    uint64_t now_ms = g_now_ns / 1000000ull;
    CHECK(deadline >= now_ms + 499u && deadline <= now_ms + 501u);
    reset_sent();
    advance_ms(deadline - now_ms - 1u);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 0u);
    advance_ms(2u);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgInputKey);
    CHECK(field_of(&g_sent[0], InteropTagPressed, 1u) == 1u);
    CHECK(field_of(&g_sent[0], InteropTagKey, 2u) == 30u);

    /* Release cancels further repeats. */
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 0u) == SessionInputPostOk);
    advance_ms(2000u);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgInputKey);
    CHECK(field_of(&g_sent[0], InteropTagPressed, 1u) == 0u);
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);

    /* A clock that moves backwards cancels the repeat without failing the run. */
    CHECK(session_input_post_key(&input, 31u, 1u) == SessionInputPostOk);
    uint64_t saved = g_now_ns;
    g_now_ns = 1000000000ull;
    CHECK(session_input_post_key(&input, 31u, 0u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_now_ns = saved;
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);

    /* A repeat that finds the queue blocked is dropped and counted. */
    CHECK(session_input_post_key(&input, 32u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_send_result = HostFeatureQueueFull;
    advance_ms(1000u);
    CHECK(tick(&handler) == 0);
    CHECK(input.repeats_dropped >= 1u);
    g_send_result = HostFeatureSuccess;
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_revoke_and_deadlines(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x6006u, 5u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);

    /* Revoke drops held and pending input without sending a release; the guest releases itself. */
    reset_sent();
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 2u) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgLeaseAck);
    CHECK(field_of(&g_sent[0], InteropTagSerial, 4u) == 4u);
    CHECK(input.held_count == 0u);
    CHECK(input.pending_count == 0u);
    CHECK(!input.forwarding);
    CHECK(handler.next_deadline_ms(handler.context, 0u) != UINT64_MAX);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 4u, 2u) == 0);

    /* After Ready, no replay: the key and the old enter do not forward until a fresh enter. */
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 0u);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x6006u, 5u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgInputFocus);
    CHECK(field_of(&g_sent[0], InteropTagLeaseEpoch, 8u) == 2u);

    /* Skipped epoch, and a second Revoked before Ready, both fail the run. */
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 9u) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 2u) != 0);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);

    /* Deadline expiry before Ready fails the run; Ready after expiry also fails. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    advance_ms(2000u);
    CHECK(tick(&handler) != 0);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    advance_ms(2000u);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
}

static void test_hostile_lease_frames(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    uint32_t serial = handshake(&handler, 1u);
    (void)serial;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 2u) == 0);
    /* Stale Ready (older epoch) is a no-op; a wrong Ack serial fails; a future epoch fails. */
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) == 0);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 99u, 2u) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 3u) != 0);
    /* Frames of an unknown type reach on_frame only by mistake and fail. */
    uint8_t frame[FrameCapacity];
    size_t length = build_guest_lease(frame, InteropMsgLeaseReady, 1, 1u, 1u);
    CHECK(deliver(&handler, InteropMsgInputKey, frame, length) != 0);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_overflow_release_all(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    reset_sent();

    /* Block the queue, enter, then press 300 distinct keys. Overflow compacts keys into a release-all. */
    g_send_result = HostFeatureQueueFull;
    CHECK(session_input_post_focus_enter(&input, 0x7007u, 6u) == SessionInputPostOk);
    for (uint32_t code = 1u; code <= 300u; code++) {
        CHECK(session_input_post_key(&input, code, 1u) == SessionInputPostOk);
    }
    CHECK(input.overflows == 1u);
    CHECK(input.release_all_pending == 1u);
    CHECK(input.pending_count < SessionInputPendingCapacity);
    g_send_result = HostFeatureSuccess;
    CHECK(tick(&handler) == 0);
    CHECK(g_sent[0].type == InteropMsgInputRelease);
    CHECK(g_sent[1].type == InteropMsgInputFocus);
    /* Keys pressed after the overflow are sent in order and are the only InputKey frames. */
    size_t key_frames = 0;
    for (size_t index = 2; index < g_sent_count; index++) {
        CHECK(g_sent[index].type == InteropMsgInputKey);
        key_frames++;
    }
    CHECK(key_frames > 0u && key_frames <= 300u);
    CHECK(input.release_all_pending == 0u);
    session_input_post_focus_leave(&input);
    tick(&handler);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_queue_capacity_fails_closed(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    g_send_result = HostFeatureQueueFull;
    int status = SessionInputPostOk;
    for (int cycle = 0; cycle < 200 && status == SessionInputPostOk; cycle++) {
        status = session_input_post_focus_enter(&input, 0x8008u, 1u);
        if (status == SessionInputPostOk) status = session_input_post_focus_leave(&input);
    }
    CHECK(status == SessionInputPostTerminal);
    CHECK(input.terminal == 1u);
    CHECK(tick(&handler) != 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostTerminal);
    g_send_result = HostFeatureSuccess;
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_sequence_wrap_and_drain(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x9009u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    /* Force the serial near the boundary: the last representable serial is still sent. */
    input.serial = UINT32_MAX - 1u;
    reset_sent();
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(field_of(&g_sent[0], InteropTagSerial, 4u) == UINT32_MAX);
    CHECK(session_input_post_key(&input, 31u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) != 0);
    CHECK(input.terminal == 1u);

    /* Draining during a send stops without failing; closing is exact-once and blocks posts. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xA00Au, 2u) == SessionInputPostOk);
    g_send_result = HostFeatureDraining;
    CHECK(tick(&handler) == 0);
    g_send_result = HostFeatureSuccess;
    reset_sent();
    handler.on_closing(handler.context, HostControlCancelled, g_sender);
    handler.on_closing(handler.context, HostControlCancelled, g_sender);
    CHECK(g_sent_count == 0u);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostClosed);
    CHECK(session_input_post_focus_enter(&input, 0xA00Au, 2u) == SessionInputPostClosed);
    CHECK(tick(&handler) != 0);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostClosed);
}

static void test_inhibit_actions(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_inhibit_actions = 0;
    CHECK(session_input_post_focus_enter(&input, 0xB00Bu, 3u) == SessionInputPostOk);
    waddle_keyboard_inhibit_event_t event;
    memset(&event, 0, sizeof event);
    event.kind = WaddleKeyboardInhibitEventSurfaceState;
    event.fullscreen = 1;
    event.window = 0xB00Bu;
    event.generation = 3u;
    CHECK(session_input_post_inhibit(&input, &event) == SessionInputPostOk);
    CHECK(g_inhibit_actions >= 1);
    CHECK(g_inhibit_last_window == 0xB00Bu);
    event.kind = WaddleKeyboardInhibitEventFocusEnter;
    CHECK(session_input_post_inhibit(&input, &event) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_inhibit(&input, NULL) == SessionInputPostInvalidArgument);
    g_inhibit_actions = 0;
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(g_inhibit_actions >= 1);
    CHECK(g_inhibit_last_kind == WaddleKeyboardInhibitActionDestroy);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static void test_random_sequence_no_stuck_keys(void) {
    session_input_t input;
    session_handler_t handler;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    uint32_t serial = handshake(&handler, 1u);
    (void)serial;
    uint64_t epoch = 1u;
    uint64_t state = 0x9e3779b97f4a7c15ull;
    uint8_t model_held[SessionInputMaxKey + 1];
    memset(model_held, 0, sizeof model_held);
    uint32_t last_serial = 0;
    size_t frames_checked = 0;
    for (int step = 0; step < 2000; step++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint32_t action = (uint32_t)(state % 100u);
        g_send_result = (state >> 20) % 10u == 0u ? HostFeatureQueueFull : HostFeatureSuccess;
        if (action < 10u) {
            CHECK(session_input_post_focus_enter(&input, 0xC00Cu, 4u) == SessionInputPostOk);
        } else if (action < 16u) {
            CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
        } else if (action < 60u) {
            uint32_t code = 1u + (uint32_t)((state >> 8) % 96u);
            CHECK(session_input_post_key(&input, code, (uint8_t)((state >> 40) & 1u)) == SessionInputPostOk);
        } else if (action < 70u) {
            CHECK(session_input_post_modifiers(&input, 0u, 0u, (uint32_t)((state >> 12) & 0x1Fu)) == SessionInputPostOk);
        } else if (action < 90u) {
            CHECK(tick(&handler) == 0);
        } else if (action < 93u) {
            advance_ms(1u + (state >> 33) % 300u);
            CHECK(tick(&handler) == 0);
        } else if (action < 95u) {
            /* Lease end: the guest releases its own keys; the model follows. */
            memset(model_held, 0, sizeof model_held);
            CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, epoch + 1u) == 0);
            epoch++;
            g_send_result = HostFeatureSuccess;
            CHECK(tick(&handler) == 0);
            if (input.phase == SessionInputAwaitReady) {
                CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, input.ack_serial, epoch) == 0);
            }
        } else {
            CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
            CHECK(session_input_post_focus_enter(&input, 0xC00Cu, 4u) == SessionInputPostOk);
        }
        /* Replay what was sent since the last check against the guest model. */
        for (; frames_checked < g_sent_count; frames_checked++) {
            const sent_frame_t *frame = &g_sent[frames_checked];
            if (frame->type == InteropMsgInputFocus) {
                if (field_of(frame, InteropTagPressed, 1u) == 0u) memset(model_held, 0, sizeof model_held);
            } else if (frame->type == InteropMsgInputKey) {
                uint32_t code = (uint32_t)field_of(frame, InteropTagKey, 2u);
                model_held[code] = (uint8_t)field_of(frame, InteropTagPressed, 1u);
            } else if (frame->type == InteropMsgInputRelease) {
                memset(model_held, 0, sizeof model_held);
            }
            if (frame->type == InteropMsgInputKey || frame->type == InteropMsgInputFocus ||
                frame->type == InteropMsgInputRelease || frame->type == InteropMsgInputLockState ||
                frame->type == InteropMsgLeaseAck) {
                uint32_t frame_serial = (uint32_t)field_of(frame, InteropTagSerial, 4u);
                CHECK(frame_serial > last_serial);
                last_serial = frame_serial;
            }
        }
    }
    /* Drain: deliver everything, close the focus, and require no key left down on the guest. */
    g_send_result = HostFeatureSuccess;
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    for (int round = 0; round < 8; round++) CHECK(tick(&handler) == 0);
    for (; frames_checked < g_sent_count; frames_checked++) {
        const sent_frame_t *frame = &g_sent[frames_checked];
        if (frame->type == InteropMsgInputKey) {
            model_held[field_of(frame, InteropTagKey, 2u)] = (uint8_t)field_of(frame, InteropTagPressed, 1u);
        } else if (frame->type == InteropMsgInputFocus && field_of(frame, InteropTagPressed, 1u) == 0u) {
            memset(model_held, 0, sizeof model_held);
        } else if (frame->type == InteropMsgInputRelease) {
            memset(model_held, 0, sizeof model_held);
        }
    }
    for (uint32_t code = 0; code <= SessionInputMaxKey; code++) CHECK(model_held[code] == 0u);
    CHECK(input.held_count == 0u);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}

static int dispatch_stub(void *context) {
    (void)context;
    g_dispatch_calls++;
    return g_dispatch_result;
}

static void test_dispatch_and_poll(void) {
    session_input_t input;
    session_handler_t handler;
    struct pollfd fds[SessionHandlerMaxDescriptors];
    size_t count = 9u;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(handler.poll_fds(handler.context, fds, SessionHandlerMaxDescriptors, &count) == 0);
    CHECK(count == 0u);
    CHECK(session_input_set_display(&input, 7, NULL, NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_set_display(&input, 7, dispatch_stub, NULL) == SessionInputPostOk);
    CHECK(handler.poll_fds(handler.context, fds, SessionHandlerMaxDescriptors, &count) == 0);
    CHECK(count == 1u && fds[0].fd == 7 && fds[0].events == POLLIN && fds[0].revents == 0);
    CHECK(handler.poll_fds(handler.context, fds, 0u, &count) != 0);
    fds[0].revents = 0;
    g_dispatch_calls = 0;
    CHECK(handler.on_fds(handler.context, fds, count, 0u, g_sender) == 0);
    CHECK(g_dispatch_calls == 0);
    fds[0].revents = POLLIN;
    CHECK(handler.on_fds(handler.context, fds, count, 0u, g_sender) == 0);
    CHECK(g_dispatch_calls == 1);
    g_dispatch_result = 1;
    CHECK(handler.on_fds(handler.context, fds, count, 0u, g_sender) != 0);
    CHECK(input.terminal == 1u);
    g_dispatch_result = 0;
    CHECK(session_input_set_display(&input, -1, NULL, NULL) == SessionInputPostOk);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
}


/** @brief Handler state with input negotiated only for KeyboardLayout (no Input capability). */
static void begin_layout_only(session_input_t *input, session_handler_t *handler) {
    reset_sent();
    g_send_result = HostFeatureSuccess;
    CHECK(session_input_init(input, WaddleKeyboardInhibitFullscreen, NULL, NULL) == SessionInputPostOk);
    *handler = session_input_make_handler(input);
    CHECK(handler->on_ready(handler->context, InteropCapKeyboardLayout, g_sender) == 0);
}

static void test_error_paths(void) {
    session_input_t input;
    session_handler_t handler;
    uint8_t frame[FrameCapacity];
    uint8_t header_only[InteropHeaderBytes];
    memset(header_only, 0, sizeof header_only);

    /* NULL state guards on every post. */
    CHECK(session_input_post_focus_leave(NULL) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_modifiers(NULL, 0u, 0u, 0u) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_keymap(NULL, NULL, 0u) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_repeat_info(NULL, 1, 1) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_inhibit(NULL, NULL) == SessionInputPostInvalidArgument);

    /* Focus guards and the duplicate-pair and wide-generation paths. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x1u, 0u) == SessionInputPostInvalidArgument);
    CHECK(session_input_post_focus_enter(&input, 0x1u, 5u) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x1u, 5u) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0x2u, 0x100000000ull) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);

    /* A lock index outside the 32 modifier bits is an internal error: the run fails. */
    CHECK(session_input_post_focus_enter(&input, 0x3u, 6u) == SessionInputPostOk);
    input.indices.caps = 40;
    CHECK(session_input_post_modifiers(&input, 0u, 0u, 0x2u) == SessionInputPostTerminal);
    CHECK(tick(&handler) != 0);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Clock failures during a key and during a deadline check. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x4u, 7u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_clock_fail = 1;
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostTerminal);
    g_clock_fail = 0;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    g_clock_fail = 1;
    CHECK(tick(&handler) != 0);
    g_clock_fail = 0;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_clock_fail = 1;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);
    g_clock_fail = 0;
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    g_clock_fail = 1;
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
    g_clock_fail = 0;

    /* An invalid clock value in the repeat service fails the run. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x4u, 7u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_clock_bad = 1;
    CHECK(tick(&handler) != 0);
    g_clock_bad = 0;

    /* A failed LeaseAck during the barrier is retried; the second failure is fatal. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_send_result = HostFeatureQueueFull;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    CHECK(input.phase == SessionInputBarrier);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(tick(&handler) != 0);
    g_send_result = HostFeatureSuccess;

    /* Malformed lease payloads: missing epoch, missing serial, short frames. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver(&handler, InteropMsgLeaseRevoked, header_only, sizeof header_only) != 0);
    CHECK(deliver(&handler, InteropMsgLeaseRevoked, header_only, 8u) != 0);
    CHECK(deliver(&handler, InteropMsgLeaseReady, header_only, sizeof header_only) != 0);
    CHECK(deliver(&handler, InteropMsgInputKey, header_only, sizeof header_only) != 0);

    /* Ready in a non-AwaitReady phase, and an epoch that cannot advance. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    input.epoch = UINT64_MAX;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);

    /* Once terminal, every callback reports failure. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
    CHECK(handler.on_ready(handler.context, InteropCapInput, g_sender) != 0);
    CHECK(handler.on_fds(handler.context, NULL, 0u, 0u, g_sender) != 0);
    CHECK(session_input_post_focus_enter(&input, 0x5u, 1u) == SessionInputPostTerminal);
    CHECK(session_input_post_repeat_info(&input, 1, 1) == SessionInputPostTerminal);

    /* Unknown message types fail the run even on a fresh handler. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(deliver(&handler, InteropMsgInputKey, frame, 0u) != 0);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, 1u, 1u) != 0);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
    CHECK(deliver(&handler, InteropMsgLeaseRevoked, frame, 0u) != 0);
    CHECK(handler.on_ready(handler.context, InteropCapInput, g_sender) != 0);
    CHECK(handler.on_fds(handler.context, NULL, 0u, 0u, g_sender) != 0);
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);

    /* Keymap variants, malformed variant, and an unchanged layout. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    const char *variant_map = "xkb_symbols \"x\" {\n    include \"pc+de(nodeadkeys)+inet(evdev)\"\n};\n";
    const char *bad_variant = "xkb_symbols \"x\" {\n    include \"pc+de(a b)\"\n};\n";
    CHECK(session_input_post_keymap(&input, (const uint8_t *)variant_map, strlen(variant_map)) == SessionInputPostOk);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)variant_map, strlen(variant_map)) == SessionInputPostOk);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)bad_variant, strlen(bad_variant)) == SessionInputPostKeymapInvalid);
    CHECK(session_input_post_keymap(&input, NULL, 0u) == SessionInputPostOk);

    /* Inhibit: an invalid event kind is refused; a missing callback is tolerated; a wide generation skips inhibit. */
    waddle_keyboard_inhibit_event_t bad_event;
    memset(&bad_event, 0, sizeof bad_event);
    bad_event.kind = 0xFFu;
    CHECK(session_input_post_inhibit(&input, &bad_event) == SessionInputPostInvalidArgument);
    CHECK(session_input_init(&input, WaddleKeyboardInhibitFullscreen, NULL, NULL) == SessionInputPostOk);
    handler = session_input_make_handler(&input);
    CHECK(handler.on_ready(handler.context, InteropCapInput | InteropCapKeyboardLayout, g_sender) == 0);
    CHECK(session_input_post_focus_enter(&input, 0x6u, 0x100000000ull) == SessionInputPostOk);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);

    /* Layout-only negotiation: the layout is held back, and no input is forwarded. */
    begin_layout_only(&input, &handler);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)variant_map, strlen(variant_map)) == SessionInputPostOk);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    reset_sent();
    CHECK(tick(&handler) == 0);
    /* KeyboardLayout needs only its own capability; InputRepeatInfo needs Input and is withheld. */
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgKeyboardLayout);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    CHECK(deliver_lease(&handler, InteropMsgLeaseReady, 1, input.ack_serial, 1u) == 0);
    CHECK(session_input_post_focus_enter(&input, 0x7u, 1u) == SessionInputPostOk);
    CHECK(!input.forwarding);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Input-only negotiation: repeat info goes out, the layout does not. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_send_result = HostFeatureSuccess;
    CHECK(session_input_post_keymap(&input, (const uint8_t *)variant_map, strlen(variant_map)) == SessionInputPostOk);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    input.layout_enabled = 0;
    reset_sent();
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 1u);
    CHECK(g_sent[0].type == InteropMsgInputRepeatInfo);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* A release-all whose serial would wrap fails the run. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x8u, 2u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    input.serial = UINT32_MAX;
    input.release_all_pending = 1;
    CHECK(tick(&handler) != 0);

    /* A transmit error other than full or closed fails the run. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0x9u, 3u) == SessionInputPostOk);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(tick(&handler) != 0);
    g_send_result = HostFeatureSuccess;

    /* A repeat that is due after a backwards clock step restarts the reducer and keeps running. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0xAu, 4u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    uint64_t saved_clock = g_now_ns;
    g_now_ns -= 100000000ull;
    CHECK(tick(&handler) == 0);
    g_now_ns = saved_clock;
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* A repeat that is due while the next serial would wrap fails the run. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0xBu, 5u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    input.serial = UINT32_MAX;
    advance_ms(1000u);
    CHECK(tick(&handler) != 0);

    /* A pending queue full of keys accepts a lock entry after compaction. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    g_send_result = HostFeatureQueueFull;
    CHECK(session_input_post_focus_enter(&input, 0xCu, 6u) == SessionInputPostOk);
    for (uint32_t code = 1u; code <= 255u; code++) CHECK(session_input_post_key(&input, code, 1u) == SessionInputPostOk);
    CHECK(input.pending_count == SessionInputPendingCapacity);
    CHECK(session_input_post_modifiers(&input, 0u, 0u, 0x2u) == SessionInputPostOk);
    CHECK(input.overflows == 1u);
    CHECK(input.pending_count == 2u);
    CHECK(input.pending[1].kind == SessionInputPendingLocks);
    g_send_result = HostFeatureSuccess;
    CHECK(tick(&handler) == 0);
    CHECK(g_sent_count == 3u);
    CHECK(g_sent[0].type == InteropMsgInputRelease);
    CHECK(g_sent[1].type == InteropMsgInputFocus);
    CHECK(g_sent[2].type == InteropMsgInputLockState);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Closed handlers reject frames, deadlines, and fds. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);
    CHECK(handler.on_fds(handler.context, NULL, 0u, 0u, g_sender) != 0);
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostClosed);
}


/** @brief Makes the pending queue exactly full of focus entries, which compaction cannot free. */
static void fill_pending_nonkey(session_input_t *input) {
    for (uint16_t index = 0; index < SessionInputPendingCapacity; index++) {
        input->pending[index].kind = SessionInputPendingFocus;
    }
    input->pending_count = SessionInputPendingCapacity;
}

static void test_error_paths_more(void) {
    session_input_t input;
    session_handler_t handler;
    struct pollfd fds[SessionHandlerMaxDescriptors];
    memset(fds, 0, sizeof fds);

    /* Release-all deferred by a full queue stays pending, then goes out. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    g_send_result = HostFeatureQueueFull;
    CHECK(session_input_post_focus_enter(&input, 0xD0u, 1u) == SessionInputPostOk);
    for (uint32_t code = 1u; code <= 300u; code++) CHECK(session_input_post_key(&input, code, 1u) == SessionInputPostOk);
    CHECK(input.release_all_pending == 1u);
    CHECK(tick(&handler) == 0);
    CHECK(input.release_all_pending == 1u);
    g_send_result = HostFeatureSuccess;
    CHECK(tick(&handler) == 0);
    CHECK(input.release_all_pending == 0u);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Layout send: failure is fatal, a full queue keeps the change for the next tick. */
    const char *keymap = "xkb_symbols \"x\" {\n    include \"pc+de+inet(evdev)\"\n};\n";
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)keymap, strlen(keymap)) == SessionInputPostOk);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(tick(&handler) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(session_input_post_keymap(&input, (const uint8_t *)keymap, strlen(keymap)) == SessionInputPostOk);
    g_send_result = HostFeatureQueueFull;
    CHECK(tick(&handler) == 0);
    CHECK(input.layout_dirty == 1u);
    g_send_result = HostFeatureSuccess;
    CHECK(tick(&handler) == 0);
    CHECK(input.layout_dirty == 0u);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Repeat info: the same split between fatal failure and a retry. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_repeat_info(&input, 10, -1) == SessionInputPostInvalidArgument);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(tick(&handler) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    g_send_result = HostFeatureQueueFull;
    CHECK(tick(&handler) == 0);
    CHECK(input.repeat_dirty == 1u);
    g_send_result = HostFeatureSuccess;
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* A due repeat is dropped while a key entry is still queued; failure and draining paths. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0xE0u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_send_result = HostFeatureQueueFull;
    CHECK(session_input_post_key(&input, 31u, 1u) == SessionInputPostOk);
    advance_ms(1000u);
    CHECK(tick(&handler) == 0);
    CHECK(input.pending_count == 1u);
    CHECK(input.repeats_dropped == 0u);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(tick(&handler) != 0);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_repeat_info(&input, 10, 500) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0xE1u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(tick(&handler) == 0);
    g_send_result = HostFeatureDraining;
    advance_ms(1000u);
    CHECK(tick(&handler) == 0);
    g_send_result = HostFeatureSuccess;
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Full queue of focus entries: each post that must enqueue fails the run. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xF0u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    /* A key on a queue full of focus entries is covered by release-all: it is dropped, nothing sticks. */
    CHECK(session_input_post_key(&input, 30u, 1u) == SessionInputPostOk);
    CHECK(input.release_all_pending == 1u);
    CHECK(input.held_count == 0u);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xF1u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    CHECK(session_input_post_modifiers(&input, 0u, 0u, 0x2u) == SessionInputPostTerminal);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xF2u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostTerminal);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xF3u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    CHECK(session_input_post_focus_enter(&input, 0xF4u, 1u) == SessionInputPostTerminal);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_enter(&input, 0xF5u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    const char *keymap_bad = "xkb_symbols \"x\" {\n    include \"pc+US\"\n};\n";
    CHECK(session_input_post_keymap(&input, (const uint8_t *)keymap_bad, strlen(keymap_bad)) == SessionInputPostTerminal);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    (void)handshake(&handler, 1u);
    CHECK(session_input_post_focus_leave(&input) == SessionInputPostOk);
    CHECK(session_input_post_focus_enter(&input, 0xF6u, 1u) == SessionInputPostOk);
    fill_pending_nonkey(&input);
    CHECK(session_input_post_focus_enter(&input, 0xF7u, 1u) == SessionInputPostTerminal);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* Inhibit: a focus-leave kind is only accepted through the focus functions. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    waddle_keyboard_inhibit_event_t leave_event;
    memset(&leave_event, 0, sizeof leave_event);
    leave_event.kind = WaddleKeyboardInhibitEventFocusLeave;
    CHECK(session_input_post_inhibit(&input, &leave_event) == SessionInputPostInvalidArgument);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* LeaseAck failure during the revoke, and a serial that cannot be used for the Ack. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_send_result = HostFeatureInvalidFrame;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    input.serial = UINT32_MAX;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) != 0);

    /* Barrier deadline is reported to the runtime while the Ack is retried. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    g_send_result = HostFeatureQueueFull;
    CHECK(deliver_lease(&handler, InteropMsgLeaseRevoked, 0, 0u, 1u) == 0);
    CHECK(handler.next_deadline_ms(handler.context, 0u) == input.handshake_deadline_ms);
    g_send_result = HostFeatureSuccess;
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* on_fds without descriptors, and with a readable descriptor but no dispatch hook. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    CHECK(handler.on_fds(handler.context, fds, 0u, 0u, g_sender) == 0);
    fds[0].fd = 9;
    fds[0].revents = POLLIN;
    CHECK(handler.on_fds(handler.context, fds, 1u, 0u, g_sender) == 0);
    handler.on_closing(handler.context, HostControlSuccess, g_sender);

    /* A failed run reports no deadline. */
    begin(&input, &handler, WaddleKeyboardInhibitFullscreen);
    input.terminal = 1;
    CHECK(handler.next_deadline_ms(handler.context, 0u) == UINT64_MAX);
}

int main(void) {
    test_descriptor_and_init();
    test_handshake_and_forwarding();
    test_release_on_leave_and_lock();
    test_layout_repeat_and_keymap();
    test_repeat_timing();
    test_revoke_and_deadlines();
    test_hostile_lease_frames();
    test_overflow_release_all();
    test_queue_capacity_fails_closed();
    test_sequence_wrap_and_drain();
    test_inhibit_actions();
    test_random_sequence_no_stuck_keys();
    test_dispatch_and_poll();
    test_error_paths();
    test_error_paths_more();
    (void)g_lock_calls;
    (void)g_dispatch_calls;
    (void)g_dispatch_result;
    puts("session_input_handler_test: all checks passed");
    return 0;
}
