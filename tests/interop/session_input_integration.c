/**
 * @file session_input_integration.c
 * @brief Host keyboard handler on the real session runtime path (contract section 12).
 *
 * The file includes the control-host fixture (session_control_host.c) the same way
 * session_app.c does. That fixture supplies the AF_VSOCK socketpair seam, the in-process
 * daemon, the mapping file and the frame helpers. The test runs session_app_run_detailed with
 * the keyboard handler as its only feature handler, so the production composition (handler
 * table, union capability offer, pump, drain and closing) is exercised.
 *
 * Fake guest: a thread on the guest end of the socketpair. It speaks the wire protocol and
 * decodes every host frame with the codec. Its decoded view (serials, held keys) is the guest
 * model that the assertions read.
 *
 * Seam: the handler's display descriptor is the read end of a nonblocking pipe, registered with
 * session_input_set_display. The guest writes one byte per scripted step. The dispatch hook
 * (called by the pump from on_fds) consumes the byte, performs exactly one post function call
 * from the step and returns. The handler then flushes inside the same callback, so the frames a
 * step produces are sent before the pump runs anything else. Posts never run on the guest thread.
 *
 * Wrapping: the handler table is a copy of session_input_make_handler's result. Its on_frame and
 * on_closing members are replaced with counting wrappers that call the original. This is allowed
 * because the copy is test-only and the handler itself is unchanged.
 */
#define main session_control_host_fixture_entry_unused
#include "session_control_host.c"
#undef main

#include "../../src/interop/session_app.h"
#include "../../src/interop/session_handler.h"
#include "../../src/interop/session_input_handler.h"

#include <stdio.h>

/* ---- Constants (values fixed by the scenarios below) ---- */

enum {
    KbFrameTimeoutMs = 5000, /**< Upper bound for one expected host frame or the end of a drain. */
    KbQuietMs = 80, /**< Window in which no host frame may arrive after a step that must not send. */
    KbLongQuietMs = 250, /**< Window after a key release in which no repeat may arrive. */
    KbKeyA = 30, /**< Evdev code of KEY_A, a repeatable key. */
    KbWindow = 0x1000, /**< Guest window of the focused pair. */
    KbGeneration = 7, /**< Guest generation of the focused pair. */
    KbLockedCapsNum = 0x12, /**< Locked bits 1 (caps) and 4 (num) under the fallback indices. */
    KbLockMask = 0x03, /**< Expected InputLockState mask: caps bit 0 and num bit 1. */
    KbRepeatRate = 50, /**< Repeats per second; the nominal period is 20 ms. */
    KbRepeatDelayMs = 100, /**< Delay before the first repeat. */
    KbRepeatsBeforeRelease = 6, /**< Repeat frames collected before the key is released. */
    KbRepeatMinGapMs = 15 /**< Lower bound for consecutive repeat arrivals (nominal 20 ms, 5 ms jitter allowance). */
};

/** @brief Keymap posted in the layout step: its include names layout "de" with no variant, and it has no modifier_map. */
static const char KbKeymapText[] = "xkb_symbols \"x\" {\n    include \"pc+de+inet(evdev)\"\n};\n";

/* ---- Failure reporting ---- */

/**
 * @brief Aborts the test with the failing expression. Used by KB_CHECK on the pump or guest thread.
 * @param[in] file Source file of the failed check.
 * @param[in] line Line of the failed check.
 * @param[in] expression Stringized expression that evaluated false.
 */
static void kb_fail(const char *file, int line, const char *expression)
{
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    abort();
}

#define KB_CHECK(expression) do { if (!(expression)) kb_fail(__FILE__, __LINE__, #expression); } while (0)

/* ---- Step driver (pump-side post calls) ---- */

/** @brief Post-function step kinds the dispatch hook can perform. */
typedef enum kb_step_kind_t {
    KbStepNone = 0, /**< Dispatch only; no post call. */
    KbStepFocusEnter, /**< session_input_post_focus_enter(KbWindow, KbGeneration). */
    KbStepFocusLeave, /**< session_input_post_focus_leave. */
    KbStepKey, /**< session_input_post_key(code, pressed). */
    KbStepModifiers, /**< session_input_post_modifiers(0, 0, locked). */
    KbStepKeymap, /**< session_input_post_keymap(KbKeymapText). */
    KbStepRepeatInfo /**< session_input_post_repeat_info(rate, delay). */
} kb_step_kind_t;

/** @brief One scripted step. Written by the guest thread before the trigger byte is sent. */
typedef struct kb_step_t {
    kb_step_kind_t kind; /**< Which post function to call. */
    uint32_t code; /**< Key step: evdev code. */
    uint8_t pressed; /**< Key step: 1 press, 0 release. */
    uint32_t locked; /**< Modifiers step: locked mask. */
    int32_t rate; /**< RepeatInfo step: rate. */
    int32_t delay_ms; /**< RepeatInfo step: delay. */
} kb_step_t;

/* ---- Shared state (pump thread writes, guest thread reads through the sync points) ---- */

static session_input_t kb_input;
static session_handler_t kb_inner; /**< Copy of session_input_make_handler's table, unwrapped. */
static session_handler_t kb_handler; /**< Table registered with the runtime (wrapped members). */
static kb_step_t kb_pending_step;
static int kb_trigger_read = -1;
static int kb_trigger_write = -1;
static atomic_uint kb_steps_run;
static atomic_uint kb_frames_handled;
static uint32_t kb_post_failures;
static uint32_t kb_closing_calls;
static uint32_t kb_closing_reason;
static pthread_t kb_pump_thread;
static void (*kb_script)(void);

/* ---- Guest model state (guest thread only) ---- */

static int kb_fd = -1;
static uint64_t kb_last_serial;
static uint32_t kb_expected_handled;
static uint32_t kb_host_frames;
static uint8_t kb_model[SessionInputHeldBytes];
static uint16_t kb_drained_types[16];
static uint32_t kb_drained_count;
static uint8_t kb_expected_klid[WaddleKeyboardLayoutsKlidBytes];
static uint64_t kb_first_repeat_ns;
static uint64_t kb_min_gap_ns;
static uint64_t kb_max_gap_ns;
static uint32_t kb_repeats_seen;

/* ---- Small helpers ---- */

/**
 * @brief Reads CLOCK_MONOTONIC in nanoseconds for timing checks.
 * @return Nanoseconds since an unspecified epoch; never fails the test on a healthy clock.
 */
static uint64_t kb_now_ns(void)
{
    struct timespec now;
    KB_CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

/**
 * @brief Counts the keys currently held in the guest model.
 * @return Number of set bits in kb_model.
 */
static uint32_t kb_held_count(void)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < SessionInputHeldBytes * 8u; ++index) {
        if (((kb_model[index >> 3] >> (index & 7u)) & 1u) != 0) ++count;
    }
    return count;
}

/**
 * @brief Reads an unsigned field into value, reporting presence.
 * @param[in] payload Validated payload bytes (borrowed).
 * @param[in] length Payload length.
 * @param[in] tag Field tag.
 * @param[in] width Expected width in bytes.
 * @param[out] value Field value when present; unchanged when absent.
 * @return 1 when present, 0 when absent. Malformed fields fail the test.
 */
static int kb_uint(const uint8_t *payload, size_t length, uint16_t tag, uint32_t width, uint64_t *value)
{
    int status = interop_field_uint(payload, length, tag, width, value);
    KB_CHECK(status == 0 || status == 1);
    return status == 0;
}

/* ---- Frame decoding (guest side, with the codec) ---- */

/** @brief Decoded host frame. Each has_ flag records presence of the matching tag. */
typedef struct kb_frame_t {
    uint16_t type; /**< Message type from the header. */
    uint32_t request_id; /**< Header request identifier. */
    uint64_t window; int has_window; /**< Window tag. */
    uint64_t generation; int has_generation; /**< Generation tag. */
    uint64_t serial; int has_serial; /**< Serial tag. */
    uint64_t pressed; int has_pressed; /**< Pressed tag. */
    uint64_t key; int has_key; /**< Key tag. */
    uint64_t lease; int has_lease; /**< Lease epoch tag. */
    uint64_t locks; int has_locks; /**< Locks tag. */
    uint64_t interval; int has_interval; /**< IntervalMs tag. */
    uint64_t rate; int has_rate; /**< Rate tag. */
    uint64_t capabilities; int has_capabilities; /**< Capabilities tag. */
    uint8_t layout[WaddleKeyboardLayoutsKlidBytes]; int has_layout; /**< Layout bytes (KLID). */
} kb_frame_t;

/**
 * @brief Decodes one validated host frame into kb_frame_t.
 * @param[in] frame Complete frame bytes (borrowed) of at least InteropHeaderBytes.
 * @param[in] length Frame length from the receive path.
 * @param[out] out Decoded view; fully written.
 */
static void kb_decode(const uint8_t *frame, size_t length, kb_frame_t *out)
{
    const uint8_t *payload = frame + InteropHeaderBytes;
    size_t payload_length = length - InteropHeaderBytes;
    const uint8_t *layout_bytes = NULL;
    uint16_t layout_length = 0;
    int layout_status;

    memset(out, 0, sizeof(*out));
    out->type = read_u16_le(frame + 6);
    out->request_id = read_u32_le(frame + 12);
    KB_CHECK(interop_payload_validate(out->type, InteropFromHost, payload_length > 0 ? payload : NULL,
        payload_length) == InteropErrorNone);
    out->has_window = kb_uint(payload, payload_length, InteropTagWindow, 8, &out->window);
    out->has_generation = kb_uint(payload, payload_length, InteropTagGeneration, 8, &out->generation);
    out->has_serial = kb_uint(payload, payload_length, InteropTagSerial, 4, &out->serial);
    out->has_pressed = kb_uint(payload, payload_length, InteropTagPressed, 1, &out->pressed);
    out->has_key = kb_uint(payload, payload_length, InteropTagKey, 2, &out->key);
    out->has_lease = kb_uint(payload, payload_length, InteropTagLeaseEpoch, 8, &out->lease);
    out->has_locks = kb_uint(payload, payload_length, InteropTagLocks, 1, &out->locks);
    out->has_interval = kb_uint(payload, payload_length, InteropTagIntervalMs, 4, &out->interval);
    out->has_rate = kb_uint(payload, payload_length, InteropTagRate, 4, &out->rate);
    out->has_capabilities = kb_uint(payload, payload_length, InteropTagCapabilities, 8, &out->capabilities);
    layout_status = interop_field_get(payload, payload_length, InteropTagLayout, 0, &layout_bytes, &layout_length);
    KB_CHECK(layout_status == 0 || layout_status == 1);
    if (layout_status == 0) {
        KB_CHECK(layout_length == WaddleKeyboardLayoutsKlidBytes);
        memcpy(out->layout, layout_bytes, WaddleKeyboardLayoutsKlidBytes);
        out->has_layout = 1;
    }
}

/**
 * @brief Applies one decoded host frame to the guest held-key model.
 * @param[in,out] frame Decoded frame; read only.
 * @note InputKey sets or clears its key. InputRelease and InputFocus with pressed 0 clear every key,
 *       which is what the guest does on a window leave.
 */
static void kb_model_apply(const kb_frame_t *frame)
{
    if (frame->type == InteropMsgInputKey && frame->has_key) {
        uint32_t code = (uint32_t)frame->key;
        if (frame->pressed != 0) {
            kb_model[code >> 3] = (uint8_t)(kb_model[code >> 3] | (1u << (code & 7u)));
        } else {
            kb_model[code >> 3] = (uint8_t)(kb_model[code >> 3] & ~(1u << (code & 7u)));
        }
    } else if (frame->type == InteropMsgInputRelease ||
            (frame->type == InteropMsgInputFocus && frame->has_pressed && frame->pressed == 0)) {
        memset(kb_model, 0, sizeof(kb_model));
    }
}

/* ---- Guest transport ---- */

/**
 * @brief Sends a complete guest frame on the socketpair.
 * @param[in] frame Borrowed frame bytes.
 * @param[in] length Exact frame size.
 */
static void kb_send(const uint8_t *frame, size_t length)
{
    write_all(kb_fd, frame, length);
}

/**
 * @brief Receives one host frame or reports a timeout or end of stream.
 * @param[out] frame Buffer of InteropMaxFrameBytes.
 * @param[out] length Frame length on success.
 * @param[in] timeout_ms Poll timeout.
 * @return 1 frame, 0 timeout, -1 end of stream or a protocol read error.
 */
static int kb_recv(uint8_t *frame, size_t *length, uint32_t timeout_ms)
{
    struct pollfd descriptor = {.fd = kb_fd, .events = POLLIN, .revents = 0};
    int ready;
    do {
        ready = poll(&descriptor, 1, (int)timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready == 0) return 0;
    KB_CHECK(ready > 0);
    if (!try_read_interop_frame(kb_fd, frame, InteropMaxFrameBytes, length)) return -1;
    return 1;
}

/**
 * @brief Answers a host Ping with a Pong carrying the same identifier.
 * @param[in] ping Complete Ping frame (borrowed).
 */
static void kb_answer_ping(const uint8_t *ping)
{
    uint8_t pong[InteropMaxFrameBytes];
    size_t pong_length = make_pong(pong, sizeof(pong), read_u32_le(ping + 12));
    kb_send(pong, pong_length);
}

/**
 * @brief Receives the next host frame that is not a heartbeat, answering heartbeats on the way.
 * @param[out] frame Buffer of InteropMaxFrameBytes.
 * @param[out] length Frame length.
 * @param[in] timeout_ms Timeout for each underlying receive.
 */
static void kb_next(uint8_t *frame, size_t *length, uint32_t timeout_ms)
{
    for (;;) {
        int status = kb_recv(frame, length, timeout_ms);
        KB_CHECK(status == 1);
        if (read_u16_le(frame + 6) != InteropMsgPing) return;
        kb_answer_ping(frame);
    }
}

/**
 * @brief Expects the next host frame to be of the given type, decodes it and checks serial monotonicity.
 * @param[in] type Expected message type.
 * @param[out] out Decoded frame.
 * @note Every serial-bearing frame must carry a serial strictly greater than the previous one.
 */
static void kb_expect(uint16_t type, kb_frame_t *out)
{
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = 0;
    kb_next(frame, &length, KbFrameTimeoutMs);
    if (read_u16_le(frame + 6) != type) {
        fprintf(stderr, "expected type 0x%04x, received 0x%04x\n", (unsigned)type,
            (unsigned)read_u16_le(frame + 6));
        abort();
    }
    kb_decode(frame, length, out);
    if (out->has_serial) {
        KB_CHECK(out->serial > kb_last_serial);
        kb_last_serial = out->serial;
    }
    kb_model_apply(out);
    ++kb_host_frames;
}

/**
 * @brief Asserts that no host frame arrives for a window (heartbeats are answered).
 * @param[in] window_ms Length of the window in milliseconds.
 */
static void kb_quiet(uint32_t window_ms)
{
    uint64_t deadline = kb_now_ns() + (uint64_t)window_ms * 1000000u;
    for (;;) {
        uint64_t now = kb_now_ns();
        uint8_t frame[InteropMaxFrameBytes];
        size_t length = 0;
        uint32_t remaining;
        int status;
        if (now >= deadline) return;
        remaining = (uint32_t)((deadline - now + 999999u) / 1000000u);
        status = kb_recv(frame, &length, remaining);
        if (status == 0) continue;
        KB_CHECK(status == 1);
        if (read_u16_le(frame + 6) == InteropMsgPing) {
            kb_answer_ping(frame);
            continue;
        }
        fprintf(stderr, "unexpected host frame 0x%04x inside a quiet window\n",
            (unsigned)read_u16_le(frame + 6));
        abort();
    }
}

/* ---- Guest expectations for the frames of the scenarios ---- */

/**
 * @brief Expects LeaseAck with the given serial and epoch (no window, the lease carries both).
 * @param[in] serial Exact serial.
 * @param[in] epoch Exact lease epoch.
 */
static void kb_expect_lease_ack(uint64_t serial, uint64_t epoch)
{
    kb_frame_t frame;
    kb_expect(InteropMsgLeaseAck, &frame);
    KB_CHECK(frame.has_serial && frame.serial == serial);
    KB_CHECK(frame.has_lease && frame.lease == epoch);
    KB_CHECK(!frame.has_window);
}

/**
 * @brief Expects an InputFocus, InputKey or InputRelease for the focused pair.
 * @param[in] type InteropMsgInputFocus, InteropMsgInputKey or InteropMsgInputRelease.
 * @param[in] pressed -1 when Pressed must be absent, else the exact 0 or 1.
 * @param[in] key 0 when Key must be absent, else the exact evdev code.
 * @param[in] serial Exact serial.
 * @param[in] lease Exact lease epoch.
 */
static void kb_expect_input(uint16_t type, int pressed, uint64_t key, uint64_t serial, uint64_t lease)
{
    kb_frame_t frame;
    kb_expect(type, &frame);
    KB_CHECK(frame.has_window && frame.window == KbWindow);
    KB_CHECK(frame.has_generation && frame.generation == KbGeneration);
    KB_CHECK(frame.has_serial && frame.serial == serial);
    KB_CHECK(frame.has_lease && frame.lease == lease);
    if (pressed >= 0) {
        KB_CHECK(frame.has_pressed && frame.pressed == (uint64_t)pressed);
    } else {
        KB_CHECK(!frame.has_pressed);
    }
    if (key != 0) {
        KB_CHECK(frame.has_key && frame.key == key);
    } else {
        KB_CHECK(!frame.has_key);
    }
}

/**
 * @brief Expects InputLockState with the exact mask, serial and lease (no window).
 * @param[in] locks Exact mask.
 * @param[in] serial Exact serial.
 * @param[in] lease Exact lease epoch.
 */
static void kb_expect_lock(uint64_t locks, uint64_t serial, uint64_t lease)
{
    kb_frame_t frame;
    kb_expect(InteropMsgInputLockState, &frame);
    KB_CHECK(frame.has_locks && frame.locks == locks);
    KB_CHECK(frame.has_serial && frame.serial == serial);
    KB_CHECK(frame.has_lease && frame.lease == lease);
    KB_CHECK(!frame.has_window);
}

/** @brief Expects KeyboardLayout with the KLID computed by keyboard_layouts for "de". No serial, no lease. */
static void kb_expect_layout(void)
{
    kb_frame_t frame;
    kb_expect(InteropMsgKeyboardLayout, &frame);
    KB_CHECK(!frame.has_serial);
    KB_CHECK(!frame.has_lease);
    KB_CHECK(frame.has_layout);
    KB_CHECK(memcmp(frame.layout, kb_expected_klid, WaddleKeyboardLayoutsKlidBytes) == 0);
}

/**
 * @brief Expects InputRepeatInfo with the exact interval and rate. No serial, no lease.
 * @param[in] interval_ms Exact delay in milliseconds.
 * @param[in] rate Exact repeats per second.
 */
static void kb_expect_repeat_info(uint64_t interval_ms, uint64_t rate)
{
    kb_frame_t frame;
    kb_expect(InteropMsgInputRepeatInfo, &frame);
    KB_CHECK(!frame.has_serial);
    KB_CHECK(!frame.has_lease);
    KB_CHECK(frame.has_interval && frame.interval == interval_ms);
    KB_CHECK(frame.has_rate && frame.rate == rate);
}

/* ---- Guest frame builders (guest direction) ---- */

/**
 * @brief Sends a feature frame the handler owns and counts it toward the on_frame wait.
 * @param[in] frame Complete frame.
 * @param[in] length Frame length.
 */
static void kb_send_owned(const uint8_t *frame, size_t length)
{
    kb_send(frame, length);
    ++kb_expected_handled;
}

/**
 * @brief Waits until the handler's on_frame wrapper has returned for every owned frame sent so far.
 * @note The wrapper increments after the handler returns, so the handler's state is settled.
 */
static void kb_wait_handled(void)
{
    uint32_t waited = 0;
    while (atomic_load(&kb_frames_handled) < kb_expected_handled) {
        KB_CHECK(waited < KbFrameTimeoutMs);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
        ++waited;
    }
}

/**
 * @brief Sends LeaseRevoked with the epoch. The guest releases its own holds first (decision 1).
 * @param[in] epoch Epoch to announce.
 */
static void kb_send_revoked(uint64_t epoch)
{
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, sizeof(frame));
    KB_CHECK(interop_put_uint(&writer, InteropTagLeaseEpoch, epoch) == InteropErrorNone);
    KB_CHECK(interop_writer_finish(&writer, InteropMsgLeaseRevoked, 0, InteropFromGuest, &length) ==
        InteropErrorNone);
    memset(kb_model, 0, sizeof(kb_model));
    kb_send_owned(frame, length);
}

/**
 * @brief Sends LeaseReady with the serial of the LeaseAck and the epoch.
 * @param[in] serial Serial the host sent in LeaseAck.
 * @param[in] epoch Epoch of the Revoked this answers.
 */
static void kb_send_ready(uint64_t serial, uint64_t epoch)
{
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, sizeof(frame));
    KB_CHECK(interop_put_uint(&writer, InteropTagSerial, serial) == InteropErrorNone);
    KB_CHECK(interop_put_uint(&writer, InteropTagLeaseEpoch, epoch) == InteropErrorNone);
    KB_CHECK(interop_writer_finish(&writer, InteropMsgLeaseReady, 0, InteropFromGuest, &length) ==
        InteropErrorNone);
    kb_send_owned(frame, length);
}

/**
 * @brief Sends CaretRect for a window and generation (the only guest frame carrying a window).
 * @param[in] window Guest window.
 * @param[in] generation Guest generation, nonzero.
 */
static void kb_send_caret(uint64_t window, uint64_t generation)
{
    uint8_t frame[InteropMaxFrameBytes];
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, sizeof(frame));
    KB_CHECK(interop_put_uint(&writer, InteropTagWindow, window) == InteropErrorNone);
    KB_CHECK(interop_put_uint(&writer, InteropTagGeneration, generation) == InteropErrorNone);
    KB_CHECK(interop_put_rect(&writer, InteropTagRect, 10, 20, 30, 40) == InteropErrorNone);
    KB_CHECK(interop_writer_finish(&writer, InteropMsgCaretRect, 0, InteropFromGuest, &length) ==
        InteropErrorNone);
    kb_send_owned(frame, length);
}

/**
 * @brief Sends a frame whose header carries an unassigned message type and an empty payload.
 * @param[in] type Unassigned message type (not a frozen interop type).
 */
static void kb_send_unknown(uint16_t type)
{
    uint8_t frame[InteropHeaderBytes];
    uint32_t magic = InteropMagic;
    uint16_t version = InteropVersion;
    memset(frame, 0, sizeof(frame));
    frame[0] = (uint8_t)magic;
    frame[1] = (uint8_t)(magic >> 8);
    frame[2] = (uint8_t)(magic >> 16);
    frame[3] = (uint8_t)(magic >> 24);
    frame[4] = (uint8_t)version;
    frame[5] = (uint8_t)(version >> 8);
    frame[6] = (uint8_t)type;
    frame[7] = (uint8_t)(type >> 8);
    kb_send(frame, sizeof(frame));
}

/* ---- Pump-side steps (dispatch hook) ---- */

/**
 * @brief Display dispatch hook. Runs on the pump thread from the handler's on_fds.
 * @param[in] context Unused; the hook reads the pending step from kb_pending_step.
 * @return Zero. A post refusal is counted in kb_post_failures and the run continues.
 */
static int kb_dispatch(void *context)
{
    uint8_t drain[16];
    int status = SessionInputPostOk;
    (void)context;
    while (read(kb_trigger_read, drain, sizeof(drain)) > 0) { }
    switch (kb_pending_step.kind) {
    case KbStepFocusEnter:
        status = session_input_post_focus_enter(&kb_input, KbWindow, KbGeneration);
        break;
    case KbStepFocusLeave:
        status = session_input_post_focus_leave(&kb_input);
        break;
    case KbStepKey:
        status = session_input_post_key(&kb_input, kb_pending_step.code, kb_pending_step.pressed);
        break;
    case KbStepModifiers:
        status = session_input_post_modifiers(&kb_input, 0, 0, kb_pending_step.locked);
        break;
    case KbStepKeymap:
        status = session_input_post_keymap(&kb_input, (const uint8_t *)KbKeymapText,
            sizeof(KbKeymapText) - 1u);
        break;
    case KbStepRepeatInfo:
        status = session_input_post_repeat_info(&kb_input, kb_pending_step.rate, kb_pending_step.delay_ms);
        break;
    case KbStepNone:
    default:
        break;
    }
    if (status != SessionInputPostOk) ++kb_post_failures;
    atomic_fetch_add(&kb_steps_run, 1);
    return 0;
}

/**
 * @brief Triggers one step on the pump and waits until the pump has performed it.
 * @param[in] step Step to perform; copied before the trigger byte is written.
 * @note Frames the step produces are sent by the handler in the same pump callback, after this
 *       returns. The caller reads them with kb_expect or proves their absence with kb_quiet.
 */
static void kb_trigger(const kb_step_t *step)
{
    unsigned int before = atomic_load(&kb_steps_run);
    uint8_t byte = 1;
    uint32_t waited = 0;
    kb_pending_step = *step;
    KB_CHECK(write(kb_trigger_write, &byte, 1) == 1);
    while (atomic_load(&kb_steps_run) == before) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        KB_CHECK(waited < KbFrameTimeoutMs);
        (void)nanosleep(&pause, NULL);
        ++waited;
    }
}

/** @brief Triggers session_input_post_focus_enter for the focused pair (KbWindow, KbGeneration). */
static void kb_step_focus_enter(void)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepFocusEnter;
    kb_trigger(&step);
}

/** @brief Triggers session_input_post_focus_leave. */
static void kb_step_focus_leave(void)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepFocusLeave;
    kb_trigger(&step);
}

/**
 * @brief Triggers session_input_post_key.
 * @param[in] code Evdev code.
 * @param[in] pressed 1 for press, 0 for release.
 */
static void kb_step_key(uint32_t code, uint8_t pressed)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepKey;
    step.code = code;
    step.pressed = pressed;
    kb_trigger(&step);
}

/**
 * @brief Triggers session_input_post_modifiers with no depressed or latched bits.
 * @param[in] locked Locked modifier mask bits.
 */
static void kb_step_modifiers(uint32_t locked)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepModifiers;
    step.locked = locked;
    kb_trigger(&step);
}

/** @brief Triggers session_input_post_keymap with KbKeymapText. */
static void kb_step_keymap(void)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepKeymap;
    kb_trigger(&step);
}

/**
 * @brief Triggers session_input_post_repeat_info.
 * @param[in] rate Repeats per second.
 * @param[in] delay_ms Delay before the first repeat in milliseconds.
 */
static void kb_step_repeat_info(int32_t rate, int32_t delay_ms)
{
    kb_step_t step;
    memset(&step, 0, sizeof(step));
    step.kind = KbStepRepeatInfo;
    step.rate = rate;
    step.delay_ms = delay_ms;
    kb_trigger(&step);
}

/* ---- Handler wrappers (test-only copies of the table) ---- */

/**
 * @brief on_frame wrapper: runs the handler, then publishes that the frame is settled.
 * @return The handler's status, unchanged.
 */
static int kb_counting_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    int status = kb_inner.on_frame(context, message_type, request_id, frame, length, sender);
    atomic_fetch_add(&kb_frames_handled, 1);
    return status;
}

/**
 * @brief on_closing wrapper: counts the call (exactly once per run is asserted) and runs the handler.
 * @param[in] context Handler state.
 * @param[in] reason Runtime reason passed through to the handler.
 * @param[in] sender Valid only during this call.
 */
static void kb_counting_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    ++kb_closing_calls;
    kb_closing_reason = reason;
    kb_inner.on_closing(context, reason, sender);
}

/* ---- Guest thread and shared scenario phases ---- */

/**
 * @brief Drains host frames until end of stream, recording every non-heartbeat type.
 * @note Used after a stop or a failure: the run closes the socket at its end.
 */
static void kb_drain_to_end(void)
{
    for (;;) {
        uint8_t frame[InteropMaxFrameBytes];
        size_t length = 0;
        int status = kb_recv(frame, &length, KbFrameTimeoutMs);
        KB_CHECK(status != 0);
        if (status < 0) return;
        if (read_u16_le(frame + 6) == InteropMsgPing) {
            kb_answer_ping(frame);
            continue;
        }
        KB_CHECK(kb_drained_count < sizeof(kb_drained_types) / sizeof(kb_drained_types[0]));
        kb_drained_types[kb_drained_count++] = read_u16_le(frame + 6);
    }
}

/**
 * @brief Handshake to Ready: Hello was sent by the guest thread; expects HelloAck, then runs the
 *        first lease exchange (Revoked epoch 1, LeaseAck serial 1, LeaseReady serial 1).
 */
static void kb_handshake_and_lease(void)
{
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = 0;
    uint64_t capabilities = 0;
    kb_next(frame, &length, KbFrameTimeoutMs);
    KB_CHECK(read_u16_le(frame + 6) == InteropMsgHelloAck);
    KB_CHECK(interop_field_uint(frame + InteropHeaderBytes, length - InteropHeaderBytes,
        InteropTagCapabilities, 8, &capabilities) == 0);
    KB_CHECK(capabilities == (InteropCapInput | InteropCapKeyboardLayout));
    kb_send_revoked(1);
    kb_expect_lease_ack(1, 1);
    kb_send_ready(1, 1);
    kb_wait_handled();
}

/** @brief Closes the guest end of the socketpair so the runtime sees end of stream; clears kb_fd. */
static void kb_close_guest(void)
{
    KB_CHECK(close(kb_fd) == 0);
    kb_fd = -1;
}

/**
 * @brief Guest script for the full scenario (contract section 12, frames a through m).
 * @note Serial numbers are asserted exactly where the sequence is fixed. Repeat frames carry
 *       strictly increasing serials and are counted, not numbered.
 */
static void kb_script_full(void)
{
    kb_frame_t frame;
    uint64_t lease_serial;
    uint64_t trigger_at;
    uint32_t repeats = 0;
    int sent_release = 0;
    int released = 0;
    uint64_t previous_arrival = 0;

    /* a, b: Hello/HelloAck/Ready with Input and KeyboardLayout, lease epoch 1. */
    kb_handshake_and_lease();

    /* c: focus enter forwards InputFocus with serial 2 and lease 1. */
    kb_step_focus_enter();
    kb_expect_input(InteropMsgInputFocus, 1, 0, 2, 1);

    /* d: key down and up, then lock state once per enter. */
    kb_step_key(KbKeyA, 1);
    kb_expect_input(InteropMsgInputKey, 1, KbKeyA, 3, 1);
    kb_step_key(KbKeyA, 0);
    kb_expect_input(InteropMsgInputKey, 0, KbKeyA, 4, 1);
    kb_step_modifiers(KbLockedCapsNum);
    kb_expect_lock(KbLockMask, 5, 1);
    kb_step_modifiers(KbLockedCapsNum);
    kb_quiet(KbQuietMs);

    /* e: keymap "de" produces KeyboardLayout with the KLID bytes; no serial. */
    kb_step_keymap();
    kb_expect_layout();

    /* f: repeat info (interval 100, rate 50), then hold a key and collect repeats. */
    kb_step_repeat_info(KbRepeatRate, KbRepeatDelayMs);
    kb_expect_repeat_info(KbRepeatDelayMs, KbRepeatRate);
    trigger_at = kb_now_ns();
    kb_step_key(KbKeyA, 1);
    kb_expect_input(InteropMsgInputKey, 1, KbKeyA, 6, 1);
    while (!released) {
        uint8_t raw[InteropMaxFrameBytes];
        size_t length = 0;
        uint64_t arrival;
        kb_next(raw, &length, KbFrameTimeoutMs);
        arrival = kb_now_ns();
        KB_CHECK(read_u16_le(raw + 6) == InteropMsgInputKey);
        kb_decode(raw, length, &frame);
        KB_CHECK(frame.has_window && frame.window == KbWindow && frame.has_generation &&
            frame.generation == KbGeneration && frame.has_key && frame.key == KbKeyA);
        KB_CHECK(frame.has_serial && frame.serial > kb_last_serial && frame.has_lease && frame.lease == 1);
        kb_last_serial = frame.serial;
        kb_model_apply(&frame);
        ++kb_host_frames;
        if (frame.pressed != 0) {
            ++repeats;
            if (repeats == 1) {
                kb_first_repeat_ns = arrival - trigger_at;
                KB_CHECK(kb_first_repeat_ns >= (uint64_t)KbRepeatDelayMs * 1000000u);
            } else {
                uint64_t gap = arrival - previous_arrival;
                KB_CHECK(gap >= (uint64_t)KbRepeatMinGapMs * 1000000u);
                if (kb_min_gap_ns == 0 || gap < kb_min_gap_ns) kb_min_gap_ns = gap;
                if (gap > kb_max_gap_ns) kb_max_gap_ns = gap;
            }
            previous_arrival = arrival;
            if (repeats == KbRepeatsBeforeRelease && !sent_release) {
                sent_release = 1;
                kb_step_key(KbKeyA, 0);
            }
        } else {
            KB_CHECK(sent_release && frame.pressed == 0);
            released = 1;
        }
    }
    kb_repeats_seen = repeats;

    /* g: release cancels repeats; nothing arrives afterwards. */
    kb_quiet(KbLongQuietMs);
    KB_CHECK(kb_held_count() == 0);

    /* h: a second lease epoch; no InputRelease on revoke. */
    lease_serial = kb_last_serial + 1;
    kb_send_revoked(2);
    kb_expect_lease_ack(lease_serial, 2);
    kb_send_ready(lease_serial, 2);
    kb_wait_handled();

    /* i: after Ready, a key down without a fresh enter is not forwarded. */
    kb_step_key(KbKeyA, 1);
    kb_quiet(KbQuietMs);
    kb_step_key(KbKeyA, 0);
    kb_quiet(KbQuietMs);
    /* A fresh enter needs a leave first: enter for the same pair while focused is a no-op. */
    kb_step_focus_leave();
    kb_quiet(KbQuietMs);
    kb_step_focus_enter();
    kb_expect_input(InteropMsgInputFocus, 1, 0, lease_serial + 1, 2);
    kb_step_modifiers(KbLockedCapsNum);
    kb_expect_lock(KbLockMask, lease_serial + 2, 2);
    kb_step_key(KbKeyA, 1);
    kb_expect_input(InteropMsgInputKey, 1, KbKeyA, lease_serial + 3, 2);

    /* j: leave while the key is held releases it, then focus-off. */
    kb_step_focus_leave();
    kb_expect_input(InteropMsgInputRelease, -1, 0, lease_serial + 4, 2);
    kb_expect_input(InteropMsgInputFocus, 0, 0, lease_serial + 5, 2);
    KB_CHECK(kb_held_count() == 0);
    kb_quiet(KbQuietMs);

    /* m: stop. The runtime drains; the handler sends nothing while closing. */
    KB_CHECK(kb_post_failures == 0);
    KB_CHECK(pthread_kill(kb_pump_thread, SIGTERM) == 0);
    kb_drain_to_end();
    KB_CHECK(kb_drained_count == 0 || (kb_drained_count == 1 && kb_drained_types[0] == InteropMsgGoodbye));
    kb_close_guest();
}

/** @brief Guest script for the CaretRect hostile case: wrong window and wrong generation are ignored. */
static void kb_script_caret(void)
{
    kb_handshake_and_lease();
    kb_step_focus_enter();
    kb_expect_input(InteropMsgInputFocus, 1, 0, 2, 1);
    kb_send_caret(0x999, KbGeneration);
    kb_wait_handled();
    kb_quiet(KbQuietMs);
    kb_send_caret(KbWindow, KbGeneration + 1);
    kb_wait_handled();
    kb_quiet(KbQuietMs);
    kb_step_key(KbKeyA, 1);
    kb_expect_input(InteropMsgInputKey, 1, KbKeyA, 3, 1);
    kb_step_key(KbKeyA, 0);
    kb_expect_input(InteropMsgInputKey, 0, KbKeyA, 4, 1);
    kb_quiet(KbQuietMs);
    kb_send_caret(KbWindow, KbGeneration);
    kb_wait_handled();
    kb_quiet(KbQuietMs);
    KB_CHECK(kb_post_failures == 0);
    KB_CHECK(pthread_kill(kb_pump_thread, SIGTERM) == 0);
    kb_drain_to_end();
    kb_close_guest();
}

/** @brief Guest script for the stale-epoch regression: Revoked(1) after epoch 2 is Ready. */
static void kb_script_stale(void)
{
    uint64_t lease_serial;
    kb_handshake_and_lease();
    lease_serial = kb_last_serial + 1;
    kb_send_revoked(2);
    kb_expect_lease_ack(lease_serial, 2);
    kb_send_ready(lease_serial, 2);
    kb_wait_handled();
    kb_send_revoked(1);
    kb_wait_handled();
    kb_drain_to_end();
    kb_close_guest();
}

/** @brief Guest script for an unassigned feature type from the guest. */
static void kb_script_unknown(void)
{
    kb_handshake_and_lease();
    kb_send_unknown(0x0FF0);
    kb_drain_to_end();
    kb_close_guest();
}

/** @brief Guest script for a known window frame the negotiated set does not admit (no Windows capability). */
static void kb_script_unnegotiated(void)
{
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;
    kb_handshake_and_lease();
    length = make_feature(frame, sizeof(frame));
    kb_send(frame, length);
    kb_drain_to_end();
    kb_close_guest();
}

/** @brief Guest script for a disconnect while a key is held. */
static void kb_script_disconnect(void)
{
    kb_handshake_and_lease();
    kb_step_focus_enter();
    kb_expect_input(InteropMsgInputFocus, 1, 0, 2, 1);
    kb_step_key(KbKeyA, 1);
    kb_expect_input(InteropMsgInputKey, 1, KbKeyA, 3, 1);
    kb_close_guest();
}

/** @brief Guest thread: connects through the vsock seam, sends Hello and runs the script. */
static void *kb_guest_main(void *unused)
{
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;
    (void)unused;
    KB_CHECK(pthread_mutex_lock(&peer_mutex) == 0);
    while (vhost_peer_fd < 0) KB_CHECK(pthread_cond_wait(&peer_condition, &peer_mutex) == 0);
    kb_fd = vhost_peer_fd;
    KB_CHECK(pthread_mutex_unlock(&peer_mutex) == 0);
    length = make_hello(frame, sizeof(frame));
    kb_send(frame, length);
    kb_script();
    if (kb_fd >= 0) kb_close_guest();
    KB_CHECK(pthread_mutex_lock(&peer_mutex) == 0);
    vhost_peer_fd = -1;
    KB_CHECK(pthread_mutex_unlock(&peer_mutex) == 0);
    return NULL;
}

/* ---- Run driver ---- */

/**
 * @brief Runs one session with the keyboard handler through session_app_run_detailed.
 * @param[in] script Guest script to run on the fake guest thread.
 * @param[out] detail Diagnostic detail from the runtime.
 * @return The runtime outcome.
 */
static interop_host_control_result_t kb_run(void (*script)(void), interop_host_control_detail_t *detail)
{
    char root[64];
    char mapping_path[96];
    int mapping_fd;
    int daemon_pair[2];
    int trigger_pair[2];
    sigset_t prior_signal_mask;
    int stop_fd;
    pthread_t daemon_thread;
    pthread_t guest_thread;
    daemon_fixture_t daemon;
    device_info_t device;
    static const uint8_t kb_agent_version[] = "1.2.3";
    interop_host_session_config_t admission;
    interop_host_control_config_t control;
    session_app_config_t app;
    interop_host_control_owners_t owners;
    interop_host_control_result_t result;

    setup_paths(root);
    KB_CHECK(snprintf(mapping_path, sizeof(mapping_path), "%s/mapping", root) > 0);
    make_mapping_file(mapping_path);
    mapping_fd = open(mapping_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    KB_CHECK(mapping_fd >= 0);
    KB_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, daemon_pair) == 0);
    stop_fd = create_stop_signal_fd(&prior_signal_mask);
    KB_CHECK(stop_fd >= 0);
    KB_CHECK(pipe2(trigger_pair, O_NONBLOCK | O_CLOEXEC) == 0);
    kb_trigger_read = trigger_pair[0];
    kb_trigger_write = trigger_pair[1];

    /* Reset per-run state. */
    kb_script = script;
    kb_last_serial = 0;
    kb_expected_handled = 0;
    kb_host_frames = 0;
    memset(kb_model, 0, sizeof(kb_model));
    kb_drained_count = 0;
    kb_first_repeat_ns = 0;
    kb_min_gap_ns = 0;
    kb_max_gap_ns = 0;
    kb_repeats_seen = 0;
    atomic_store(&kb_steps_run, 0);
    atomic_store(&kb_frames_handled, 0);
    kb_post_failures = 0;
    kb_closing_calls = 0;
    kb_closing_reason = 0;
    memset(&kb_pending_step, 0, sizeof(kb_pending_step));

    /* Handler: init, display registration, and the wrapped table the runtime receives. */
    KB_CHECK(session_input_init(&kb_input, WaddleKeyboardInhibitFullscreen, NULL, NULL) == SessionInputPostOk);
    KB_CHECK(session_input_set_display(&kb_input, kb_trigger_read, kb_dispatch, NULL) == SessionInputPostOk);
    kb_inner = session_input_make_handler(&kb_input);
    kb_handler = kb_inner;
    kb_handler.on_frame = kb_counting_on_frame;
    kb_handler.on_closing = kb_counting_on_closing;

    /* Expected KLID for layout "de", computed with the same reducer the handler uses. */
    {
        waddle_keyboard_layouts_state_t layouts;
        waddle_keyboard_layouts_outcome_t outcome;
        memset(&layouts, 0, sizeof(layouts));
        memset(&outcome, 0, sizeof(outcome));
        KB_CHECK(waddle_keyboard_layouts_observe(&layouts, (const uint8_t *)"de", 2u, NULL, 0u, &outcome) ==
            WaddleKeyboardLayoutsOk);
        KB_CHECK(outcome.kind == WaddleKeyboardLayoutsSend);
        memcpy(kb_expected_klid, outcome.klid.bytes, WaddleKeyboardLayoutsKlidBytes);
    }

    /* Daemon, peer hello and the in-process vsock seam (fixture globals). */
    daemon.fd = daemon_pair[1];
    daemon.close_after_ready = false;
    daemon.send_unsolicited = false;
    KB_CHECK(pthread_create(&daemon_thread, NULL, daemon_server, &daemon) == 0);
    peer_hello_capabilities = InteropCapInput | InteropCapKeyboardLayout;
    peer_expected_ack_capabilities = InteropCapInput | InteropCapKeyboardLayout;
    peer_bad_epoch = false;
    peer_delay_hello_ms = 0;
    peer_disconnect_after_hello = false;
    peer_send_feature = false;
    peer_send_error = false;
    peer_send_goodbye = false;
    peer_agent_version = "1.2.3";
    peer_script_enabled = false;
    vhost_connect_count = 0;
    runtime_thread_known = false;
    KB_CHECK(pthread_create(&guest_thread, NULL, kb_guest_main, NULL) == 0);

    memset(&device, 0, sizeof(device));
    strcpy(device.name, "session-control-test");
    KB_CHECK(daemon_device_get_state_dir(device.name, device.state_dir, sizeof(device.state_dir)) == 0);
    KB_CHECK(daemon_device_get_socket_path(device.name, device.socket_path, sizeof(device.socket_path)) == 0);
    strcpy(device.config_path, "/tmp/session-control-test.ini");
    strcpy(device.disk_image, "/tmp/session-control-test.qcow2");
    device.vsock_cid = 33;
    device.vsock_port = 5242;
    device.memory_mb = 4096;
    device.vcpus = 4;
    device.config_valid = 1;

    /* session_app supplies the offer; the admission and feature_ops fields must be zero here. */
    memset(&admission, 0, sizeof(admission));
    admission.device = &device;
    admission.daemon_fd = daemon_pair[0];
    admission.agent_version = kb_agent_version;
    admission.agent_version_bytes = sizeof(kb_agent_version) - 1;
    admission.connection_token = UINT64_C(0x1122334455667788);
    admission.local_capabilities = 0;
    admission.deadline_ms = monotonic_ms() + 5000;
    admission.cancel_fd = stop_fd;
    admission.guest_ready_timeout_ms = 1000;
    admission.drain_timeout_ms = 1000;
    admission.reserved_zero = 0;

    memset(&control, 0, sizeof(control));
    control.admission = &admission;
    control.mapping_fd = mapping_fd;
    control.backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    control.stop_fd = stop_fd;
    control.run_for_ms = 0;
    control.reserved_zero = 0;
    control.feature_ops = NULL;

    memset(&app, 0, sizeof(app));
    app.control = control;
    app.handlers = &kb_handler;
    app.handler_count = 1;

    memset(&owners, 0, sizeof(owners));
    if (detail != NULL) memset(detail, 0, sizeof(*detail));
    runtime_stop_fd = stop_fd;
    runtime_daemon_fd = daemon_pair[0];
    kb_pump_thread = pthread_self();
    result = session_app_run_detailed(&app, &owners, detail);
    runtime_stop_fd = -1;
    runtime_daemon_fd = -1;
    KB_CHECK(owners.mapping_owner == NULL && owners.session_owner == NULL);

    KB_CHECK(pthread_join(guest_thread, NULL) == 0);
    KB_CHECK(vhost_connect_count == 1);
    close(daemon_pair[0]);
    KB_CHECK(pthread_join(daemon_thread, NULL) == 0);
    close(daemon_pair[1]);
    close(trigger_pair[0]);
    close(trigger_pair[1]);
    kb_trigger_read = -1;
    kb_trigger_write = -1;
    {
        struct signalfd_siginfo pending_signal;
        (void)read(stop_fd, &pending_signal, sizeof(pending_signal));
        close(stop_fd);
    }
    KB_CHECK(pthread_sigmask(SIG_SETMASK, &prior_signal_mask, NULL) == 0);
    close(mapping_fd);
    KB_CHECK(unsetenv("XDG_STATE_HOME") == 0);
    KB_CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
    KB_CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    return result;
}

/** @brief Prints the drained host frame types of a run, for the contract record. */
static void kb_print_drained(const char *name)
{
    uint32_t index;
    printf("%s: drained host frame types:", name);
    if (kb_drained_count == 0) printf(" none");
    for (index = 0; index < kb_drained_count; ++index) printf(" 0x%04x", (unsigned)kb_drained_types[index]);
    printf("\n");
}

int main(void)
{
    interop_host_control_detail_t detail;
    interop_host_control_result_t result;

    KB_CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);

    /* Main scenario: frames a through m, ending with a graceful stop. */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_full, &detail);
    KB_CHECK(result == HostControlCancelled);
    KB_CHECK(detail.reached_ready == 1);
    KB_CHECK(kb_closing_calls == 1);
    KB_CHECK(kb_closing_reason == (uint32_t)result);
    KB_CHECK(kb_held_count() == 0);
    printf("full: outcome %d (HostControlCancelled), ready at %llu ms, closing %u, host frames %u\n",
        (int)result, (unsigned long long)detail.ready_at_ms, kb_closing_calls, kb_host_frames);
    printf("full: first repeat %.3f ms after key down; repeats %u; gaps min %.3f ms max %.3f ms\n",
        (double)kb_first_repeat_ns / 1e6, kb_repeats_seen, (double)kb_min_gap_ns / 1e6,
        (double)kb_max_gap_ns / 1e6);
    kb_print_drained("full");

    /* CaretRect with wrong window and generation: no effect, the run continues to a stop. */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_caret, &detail);
    KB_CHECK(result == HostControlCancelled);
    KB_CHECK(kb_closing_calls == 1);
    printf("caret: outcome %d (HostControlCancelled), closing %u\n", (int)result, kb_closing_calls);

    /* Stale epoch regression after epoch 2 is Ready: the handler fails the run, which drains. */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_stale, &detail);
    KB_CHECK(result == HostControlFeatureFailed);
    KB_CHECK(kb_closing_calls == 1);
    KB_CHECK(kb_closing_reason == (uint32_t)result);
    KB_CHECK(kb_drained_count == 1 && kb_drained_types[0] == InteropMsgGoodbye);
    printf("stale: outcome %d (HostControlFeatureFailed), closing %u, closing reason %u\n",
        (int)result, kb_closing_calls, (unsigned)kb_closing_reason);
    kb_print_drained("stale");

    /* Unassigned feature type: rejected by the frame codec before admission, Error(UnsupportedType). */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_unknown, &detail);
    KB_CHECK(result == HostControlPeerFailed);
    KB_CHECK(kb_closing_calls == 1);
    KB_CHECK(kb_closing_reason == (uint32_t)result);
    KB_CHECK(kb_drained_count == 1 && kb_drained_types[0] == InteropMsgError);
    printf("unknown: outcome %d (HostControlPeerFailed), closing %u, guest_error %u\n", (int)result,
        kb_closing_calls, (unsigned)detail.guest_error);
    kb_print_drained("unknown");

    /* Known window frame that the negotiated set does not admit: refused as unsupported, then drained. */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_unnegotiated, &detail);
    KB_CHECK(result == HostControlUnexpectedFeature);
    KB_CHECK(kb_closing_calls == 1);
    KB_CHECK(kb_closing_reason == (uint32_t)result);
    printf("unnegotiated: outcome %d (HostControlUnexpectedFeature), closing %u\n", (int)result,
        kb_closing_calls);
    kb_print_drained("unnegotiated");

    /* Guest disconnect while a key is held: the runtime sees transport loss without a drain. */
    memset(&detail, 0, sizeof(detail));
    result = kb_run(kb_script_disconnect, &detail);
    KB_CHECK(result == HostControlPeerFailed);
    KB_CHECK(kb_closing_calls == 1);
    KB_CHECK(kb_closing_reason == (uint32_t)result);
    printf("disconnect: outcome %d (HostControlPeerFailed), closing %u, closing reason %u\n",
        (int)result, kb_closing_calls, (unsigned)kb_closing_reason);

    printf("session_input_integration: all scenarios completed\n");
    return 0;
}
