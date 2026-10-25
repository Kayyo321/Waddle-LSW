/**
 * @file session_input_handler.c
 * @brief Host keyboard feature handler: lease barrier, forwarding, lock, layout, repeat, inhibit.
 */
#define _POSIX_C_SOURCE 200809L

#include "session_input_handler.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#include "keymap_names.h"
#include "waddle/interop_protocol.h"

/** @brief Bytes reserved for one outbound frame: header plus the largest fixed field set. */
enum { SessionInputFrameBytes = 160 };

/** @brief Transmit outcome of one frame. */
enum {
    TransmitSent = 0, /**< The peer queued the frame; the caller commits its serial. */
    TransmitDeferred = 1, /**< Queue full; retry on a later tick. */
    TransmitStopped = 2, /**< Closing or draining; nothing more is sent, not a failure. */
    TransmitFailed = 3 /**< Protocol or internal failure; the run must fail. */
};

/** @brief Outcome of enqueue. */
enum {
    EnqueueAppended = 0, /**< Entry stored at the tail. */
    EnqueueCovered = 1, /**< A key entry was not stored: release-all covers it. */
    EnqueueFailed = 2 /**< Queue full and nothing could be released; the handler is terminal. */
};

/* ------------------------------------------------------------------------- */
/* Clock                                                                      */
/* ------------------------------------------------------------------------- */

/** @brief Reads CLOCK_MONOTONIC in nanoseconds. @return 0 on success, -1 on failure. */
static int clock_ns(uint64_t *out_ns) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    /* A negative nanosecond field converts to a value above the valid range and is rejected. */
    if ((uint64_t)now.tv_nsec >= 1000000000ull) return -1;
    *out_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    return 0;
}

/** @brief Reads CLOCK_MONOTONIC in milliseconds, rounded down. @return 0 on success, -1 on failure. */
static int clock_ms(uint64_t *out_ms) {
    uint64_t ns = 0;
    if (clock_ns(&ns) != 0) return -1;
    *out_ms = ns / 1000000ull;
    return 0;
}

/** @brief Converts an absolute nanosecond deadline to milliseconds, rounding up (never early). */
static uint64_t deadline_ms_ceil(uint64_t deadline_ns) {
    if (deadline_ns > UINT64_MAX - 999999ull) return UINT64_MAX;
    return (deadline_ns + 999999ull) / 1000000ull;
}

/* ------------------------------------------------------------------------- */
/* Held set and pending queue                                                 */
/* ------------------------------------------------------------------------- */

/** @brief True when the forwarded key `code` is currently down. Codes are 1..767. */
static int held_test(const session_input_t *input, uint32_t code) {
    return (input->held[code >> 3] >> (code & 7u)) & 1u;
}

/** @brief Marks `code` down. Callers set only codes that are not already held. */
static void held_set(session_input_t *input, uint32_t code) {
    input->held[code >> 3] = (uint8_t)(input->held[code >> 3] | (1u << (code & 7u)));
    input->held_count++;
}

/** @brief Clears every held key without sending anything. */
static void held_clear_all(session_input_t *input) {
    memset(input->held, 0, sizeof input->held);
    input->held_count = 0;
}

/**
 * @brief Overflow resync: drops every queued key entry (release-all covers them) and sets the
 *        release-all flag so the next flush sends one InputRelease for the requested pair.
 */
static void compact_keys(session_input_t *input) {
    uint16_t write = 0;
    for (uint16_t read = 0; read < input->pending_count; read++) {
        if (input->pending[read].kind != SessionInputPendingKey) {
            input->pending[write] = input->pending[read];
            write++;
        }
    }
    input->pending_count = write;
    held_clear_all(input);
    input->release_all_pending = 1;
    input->overflows++;
}

/**
 * @brief Appends one entry, resolving a full queue by release-all compaction.
 * @return EnqueueAppended, EnqueueCovered (key dropped; release-all covers it), or EnqueueFailed
 *         (queue still full of focus and lock entries; the handler becomes terminal).
 */
static int enqueue(session_input_t *input, const session_input_pending_t *entry) {
    if (input->pending_count < SessionInputPendingCapacity) {
        input->pending[input->pending_count] = *entry;
        input->pending_count++;
        return EnqueueAppended;
    }
    compact_keys(input);
    if (entry->kind == SessionInputPendingKey) return EnqueueCovered;
    if (input->pending_count < SessionInputPendingCapacity) {
        input->pending[input->pending_count] = *entry;
        input->pending_count++;
        return EnqueueAppended;
    }
    input->terminal = 1;
    input->phase = SessionInputTerminal;
    return EnqueueFailed;
}

/** @brief Removes the head entry after it was sent. */
static void dequeue_head(session_input_t *input) {
    memmove(&input->pending[0], &input->pending[1],
        (size_t)(input->pending_count - 1) * sizeof input->pending[0]);
    input->pending_count--;
}

/* ------------------------------------------------------------------------- */
/* Frame encoding and transmission                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Sends one frame through the runtime sender and maps the result.
 * @param[in,out] input State whose sender is valid for the running callback.
 * @param[in] frame Complete frame bytes.
 * @param[in] length Exact frame size.
 * @return TransmitSent, TransmitDeferred, TransmitStopped or TransmitFailed.
 */
static int transmit(session_input_t *input, const uint8_t *frame, size_t length) {
    switch (interop_host_feature_send(input->sender, frame, length)) {
    case HostFeatureSuccess:
        return TransmitSent;
    case HostFeatureQueueFull:
        return TransmitDeferred;
    case HostFeatureDraining:
    case HostFeatureClosed:
        return TransmitStopped;
    default:
        return TransmitFailed;
    }
}

/** @brief Returns the next protocol serial without committing it. @return 0 or -1 on wrap. */
static int next_serial(const session_input_t *input, uint32_t *out_serial) {
    if (input->serial == UINT32_MAX) return -1;
    *out_serial = input->serial + 1u;
    return 0;
}

/** @brief Finalizes a frame built by the writer. @return 0 on success, -1 on encoding failure. */
static int finish_frame(interop_writer_t *writer, uint16_t kind, uint8_t *frame, size_t *out_length) {
    (void)frame;
    size_t length = 0;
    if (interop_writer_finish(writer, kind, 0u, InteropFromHost, &length) != InteropErrorNone) return -1;
    *out_length = length;
    return 0;
}

/**
 * @brief Encodes an input head frame with a serial. Fields are written in ascending tag order.
 * @param kind InteropMsgInputFocus, InteropMsgInputKey or InteropMsgInputRelease.
 * @param has_pressed Nonzero to write Pressed.
 * @param has_key Nonzero to write Key (InputKey only).
 */
static int encode_head(uint8_t *frame, size_t *out_length, uint16_t kind, uint64_t window,
    uint64_t generation, uint32_t serial, uint64_t epoch, int has_pressed, uint8_t pressed,
    int has_key, uint16_t key) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, SessionInputFrameBytes);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagSerial, serial);
    if (has_pressed) interop_put_uint(&writer, InteropTagPressed, pressed);
    if (has_key) interop_put_uint(&writer, InteropTagKey, key);
    interop_put_uint(&writer, InteropTagLeaseEpoch, epoch);
    return finish_frame(&writer, kind, frame, out_length);
}

/** @brief Sends the frame for one queued entry of the requested pair or lock state. */
static int send_pending(session_input_t *input, const session_input_pending_t *entry, uint32_t serial) {
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    int built = 0;
    if (entry->kind == SessionInputPendingKey) {
        built = encode_head(frame, &length, InteropMsgInputKey, entry->window, entry->generation,
            serial, input->epoch, 1, entry->pressed, 1, entry->code);
    } else if (entry->kind == SessionInputPendingFocus) {
        built = encode_head(frame, &length, InteropMsgInputFocus, entry->window, entry->generation,
            serial, input->epoch, 1, entry->pressed, 0, 0);
    } else if (entry->kind == SessionInputPendingRelease) {
        built = encode_head(frame, &length, InteropMsgInputRelease, entry->window, entry->generation,
            serial, input->epoch, 0, 0, 0, 0);
    } else {
        /* SessionInputPendingLocks is the only remaining kind. */
        interop_writer_t writer;
        interop_writer_init(&writer, frame, SessionInputFrameBytes);
        interop_put_uint(&writer, InteropTagSerial, serial);
        interop_put_uint(&writer, InteropTagLocks, entry->locks);
        interop_put_uint(&writer, InteropTagLeaseEpoch, input->epoch);
        built = finish_frame(&writer, InteropMsgInputLockState, frame, &length);
    }
    if (built != 0) return TransmitFailed;
    return transmit(input, frame, length);
}

/** @brief Sends the release-all frame for the requested pair. Skips when no pair was ever requested. */
static int send_release_all(session_input_t *input) {
    uint32_t serial = 0;
    if (next_serial(input, &serial) != 0) return TransmitFailed;
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    if (encode_head(frame, &length, InteropMsgInputRelease, input->pair_window,
            input->pair_generation, serial, input->epoch, 0, 0, 0, 0) != 0) {
        return TransmitFailed;
    }
    int rc = transmit(input, frame, length);
    if (rc == TransmitSent) {
        input->serial = serial;
        input->release_all_pending = 0;
    }
    return rc;
}

/** @brief Sends the LeaseAck with the next serial and records it. */
static int send_ack(session_input_t *input) {
    uint32_t serial = 0;
    if (next_serial(input, &serial) != 0) return TransmitFailed;
    interop_writer_t writer;
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    interop_writer_init(&writer, frame, SessionInputFrameBytes);
    interop_put_uint(&writer, InteropTagSerial, serial);
    interop_put_uint(&writer, InteropTagLeaseEpoch, input->epoch);
    if (finish_frame(&writer, InteropMsgLeaseAck, frame, &length) != 0) return TransmitFailed;
    int rc = transmit(input, frame, length);
    if (rc == TransmitSent) {
        input->serial = serial;
        input->ack_serial = serial;
        input->phase = SessionInputAwaitReady;
    }
    return rc;
}

/** @brief Sends KeyboardLayout for the last observed KLID. No serial, no lease. */
static int send_layout(session_input_t *input) {
    interop_writer_t writer;
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    interop_writer_init(&writer, frame, SessionInputFrameBytes);
    interop_put_bytes(&writer, InteropTagLayout, input->klid, WaddleKeyboardLayoutsKlidBytes);
    if (finish_frame(&writer, InteropMsgKeyboardLayout, frame, &length) != 0) return TransmitFailed;
    return transmit(input, frame, length);
}

/** @brief Sends InputRepeatInfo with the last values. No serial, no lease. */
static int send_repeat_info(session_input_t *input) {
    interop_writer_t writer;
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    interop_writer_init(&writer, frame, SessionInputFrameBytes);
    interop_put_uint(&writer, InteropTagIntervalMs, (uint64_t)input->repeat_delay_ms);
    interop_put_uint(&writer, InteropTagRate, (uint64_t)input->repeat_rate);
    if (finish_frame(&writer, InteropMsgInputRepeatInfo, frame, &length) != 0) return TransmitFailed;
    return transmit(input, frame, length);
}

/**
 * @brief Sends queued work in order: release-all, then pending entries, then layout and repeat info.
 * @return 0 when the queue drained or a retry is scheduled, SessionInputCallbackFailed on failure.
 */
static int flush(session_input_t *input) {
    if (input->release_all_pending) {
        int rc = send_release_all(input);
        if (rc == TransmitFailed) goto fail;
        if (rc != TransmitSent) return 0;
    }
    while (input->pending_count > 0) {
        uint32_t serial = 0;
        if (next_serial(input, &serial) != 0) goto fail;
        int rc = send_pending(input, &input->pending[0], serial);
        if (rc == TransmitFailed) goto fail;
        if (rc != TransmitSent) return 0;
        input->serial = serial;
        dequeue_head(input);
    }
    if (input->layout_dirty && input->layout_enabled && input->klid_known) {
        int rc = send_layout(input);
        if (rc == TransmitFailed) goto fail;
        if (rc == TransmitSent) input->layout_dirty = 0;
    }
    if (input->repeat_dirty && input->input_enabled && input->repeat_known) {
        int rc = send_repeat_info(input);
        if (rc == TransmitFailed) goto fail;
        if (rc == TransmitSent) input->repeat_dirty = 0;
    }
    return 0;
fail:
    input->terminal = 1;
    input->phase = SessionInputTerminal;
    return SessionInputCallbackFailed;
}

/* ------------------------------------------------------------------------- */
/* Repeat, handshake and inhibit helpers                                      */
/* ------------------------------------------------------------------------- */

/** @brief Cancels any armed repeat and forgets its focus (cancel clears the reducer's window). */
static void repeat_stop(session_input_t *input) {
    (void)waddle_keyboard_repeat_cancel(&input->repeat);
}

/**
 * @brief Recovers the repeat reducer after a clock discontinuity. Cancel clears its window, so
 *        while the pair is still forwarded the window is focused again; keys held across the
 *        discontinuity are not rearmed until pressed again.
 */
static void repeat_restart(session_input_t *input) {
    repeat_stop(input);
    if (input->forwarding) {
        (void)waddle_keyboard_repeat_focus(&input->repeat, input->pair_window, input->pair_generation);
    }
}

/** @brief Releases the lock reducer's focus state. */
static void lock_stop(session_input_t *input) {
    (void)waddle_keyboard_lock_host_leave(&input->lock_host);
}

/**
 * @brief Services the repeat timer: consumes at most one due repeat and sends it when still
 *        forwardable. A repeat is dropped when the queue is blocked or a release is pending.
 * @return 0, or SessionInputCallbackFailed on a send failure.
 */
static int service_repeat(session_input_t *input) {
    uint64_t now_ns = 0;
    if (clock_ns(&now_ns) != 0) goto fail;
    uint16_t code = 0;
    /* Only ClockDiscontinuity can come back here; the reducer then forgets its arming. */
    if (waddle_keyboard_repeat_take_due(&input->repeat, now_ns, &code) != WaddleKeyboardRepeatOk) {
        repeat_restart(input);
        return 0;
    }
    if (code == 0) return 0;
    if (!input->forwarding || input->pending_count != 0 || !held_test(input, code)) return 0;
    uint32_t serial = 0;
    if (next_serial(input, &serial) != 0) goto fail;
    uint8_t frame[SessionInputFrameBytes];
    size_t length = 0;
    if (encode_head(frame, &length, InteropMsgInputKey, input->pair_window, input->pair_generation,
            serial, input->epoch, 1, 1, 1, code) != 0) {
        goto fail;
    }
    int rc = transmit(input, frame, length);
    if (rc == TransmitFailed) goto fail;
    if (rc == TransmitDeferred) {
        input->repeats_dropped++;
        return 0;
    }
    if (rc == TransmitSent) input->serial = serial;
    return 0;
fail:
    input->terminal = 1;
    input->phase = SessionInputTerminal;
    return SessionInputCallbackFailed;
}

/**
 * @brief Checks the handshake deadline and performs the retryable barrier work, then flushes.
 * @return 0, or SessionInputCallbackFailed on expiry or a send failure.
 */
static int service(session_input_t *input) {
    if (input->phase == SessionInputBarrier || input->phase == SessionInputAwaitReady) {
        uint64_t now_ms = 0;
        if (clock_ms(&now_ms) != 0 || now_ms >= input->handshake_deadline_ms) {
            input->terminal = 1;
            input->phase = SessionInputTerminal;
            return SessionInputCallbackFailed;
        }
    }
    if (input->phase == SessionInputBarrier) {
        int rc = send_ack(input);
        if (rc == TransmitFailed) {
            input->terminal = 1;
            input->phase = SessionInputTerminal;
            return SessionInputCallbackFailed;
        }
    }
    if (flush(input) != 0) return SessionInputCallbackFailed;
    return service_repeat(input);
}

/** @brief Applies one inhibit event and forwards the actions to the Wayland glue. */
static int inhibit_apply(session_input_t *input, const waddle_keyboard_inhibit_event_t *event) {
    waddle_keyboard_inhibit_result_t result;
    memset(&result, 0, sizeof result);
    if (waddle_keyboard_inhibit_apply(&input->inhibit, event, &result) != WaddleKeyboardInhibitOk) {
        return SessionInputPostInvalidArgument;
    }
    if (input->inhibit_fn != NULL) input->inhibit_fn(input->inhibit_context, &result);
    return SessionInputPostOk;
}

/** @brief Reports a focus transition to the inhibit reducer when the generation fits its 32-bit field. */
static int inhibit_focus(session_input_t *input, uint8_t kind, uint64_t window, uint64_t generation) {
    if (generation > UINT32_MAX) return SessionInputPostOk;
    waddle_keyboard_inhibit_event_t event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.mode = input->inhibit_mode;
    event.window = window;
    event.generation = (uint32_t)generation;
    return inhibit_apply(input, &event);
}

/* ------------------------------------------------------------------------- */
/* Forwarding state transitions                                               */
/* ------------------------------------------------------------------------- */

/** @brief Begins forwarding for a pair: lock sync is armed and the repeat reducer is focused. */
static int start_forwarding(session_input_t *input, uint64_t window, uint64_t generation) {
    input->forwarding = 1;
    input->pair_window = window;
    input->pair_generation = generation;
    (void)waddle_keyboard_lock_host_enter(&input->lock_host, &input->indices);
    (void)waddle_keyboard_repeat_focus(&input->repeat, window, generation);
    session_input_pending_t entry;
    memset(&entry, 0, sizeof entry);
    entry.kind = SessionInputPendingFocus;
    entry.pressed = 1;
    entry.window = window;
    entry.generation = generation;
    return enqueue(input, &entry) == EnqueueFailed ? SessionInputPostTerminal : SessionInputPostOk;
}

/**
 * @brief Stops forwarding for the requested pair: release-all for held keys, then focus-off.
 *        Used by focus loss, keymap rejection, and overflow-free leave. Does not touch physical focus.
 */
static int stop_forwarding(session_input_t *input) {
    if (!input->forwarding) return SessionInputPostOk;
    session_input_pending_t entry;
    memset(&entry, 0, sizeof entry);
    entry.window = input->pair_window;
    entry.generation = input->pair_generation;
    if (input->held_count > 0) {
        entry.kind = SessionInputPendingRelease;
        if (enqueue(input, &entry) == EnqueueFailed) return SessionInputPostTerminal;
    }
    entry.kind = SessionInputPendingFocus;
    entry.pressed = 0;
    if (enqueue(input, &entry) == EnqueueFailed) return SessionInputPostTerminal;
    input->forwarding = 0;
    held_clear_all(input);
    repeat_stop(input);
    lock_stop(input);
    return SessionInputPostOk;
}

/** @brief Physical focus leave: stops forwarding, then clears the focused pair. */
static int leave_focus(session_input_t *input) {
    if (!input->focused) return SessionInputPostOk;
    int status = stop_forwarding(input);
    if (status != SessionInputPostOk) return status;
    input->focused = 0;
    /* An inhibit reducer refusal never gates keyboard forwarding (see the header note). */
    (void)inhibit_focus(input, WaddleKeyboardInhibitEventFocusLeave, input->focus_window, input->focus_generation);
    return SessionInputPostOk;
}

/* ------------------------------------------------------------------------- */
/* Post functions                                                             */
/* ------------------------------------------------------------------------- */

/** @brief Shared guard for every post: NULL, closed, and terminal states. */
static int post_guard(const session_input_t *input) {
    if (input == NULL) return SessionInputPostInvalidArgument;
    if (input->closed) return SessionInputPostClosed;
    if (input->terminal) return SessionInputPostTerminal;
    return SessionInputPostOk;
}

int session_input_init(session_input_t *input, uint8_t inhibit_mode,
    session_input_inhibit_fn inhibit_fn, void *inhibit_context) {
    if (input == NULL || inhibit_mode > WaddleKeyboardInhibitFocused) return SessionInputPostInvalidArgument;
    memset(input, 0, sizeof *input);
    input->inhibit_mode = inhibit_mode;
    input->inhibit_fn = inhibit_fn;
    input->inhibit_context = inhibit_context;
    input->display_fd = -1;
    input->indices.caps = 1;
    input->indices.num = 4;
    input->indices.scroll = WaddleKeyboardLockModifierAbsent;
    (void)waddle_keyboard_repeat_init(&input->repeat);
    (void)waddle_keyboard_inhibit_init(&input->inhibit);
    input->inhibit.config = inhibit_mode;
    return SessionInputPostOk;
}

int session_input_set_display(session_input_t *input, int display_fd,
    session_input_dispatch_fn dispatch, void *dispatch_context) {
    if (input == NULL) return SessionInputPostInvalidArgument;
    if (display_fd >= 0 && dispatch == NULL) return SessionInputPostInvalidArgument;
    input->display_fd = display_fd;
    input->dispatch = dispatch;
    input->dispatch_context = dispatch_context;
    return SessionInputPostOk;
}

int session_input_post_focus_enter(session_input_t *input, uint64_t window, uint64_t generation) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (generation == 0) return SessionInputPostInvalidArgument;
    if (input->focused) {
        if (input->focus_window == window && input->focus_generation == generation) return SessionInputPostOk;
        int left = leave_focus(input);
        if (left != SessionInputPostOk) return left;
    }
    input->focused = 1;
    input->focus_window = window;
    input->focus_generation = generation;
    /* An inhibit reducer refusal never gates keyboard forwarding (see the header note). */
    (void)inhibit_focus(input, WaddleKeyboardInhibitEventFocusEnter, window, generation);
    if (input->phase == SessionInputReady && input->input_enabled) {
        return start_forwarding(input, window, generation);
    }
    return SessionInputPostOk;
}

int session_input_post_focus_leave(session_input_t *input) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    return leave_focus(input);
}

int session_input_post_key(session_input_t *input, uint32_t code, uint8_t pressed) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (code < 1u || code > (uint32_t)SessionInputMaxKey) return SessionInputPostInvalidKey;
    if (!input->forwarding) return SessionInputPostOk;
    uint8_t pressed_bit = pressed ? 1u : 0u;
    if (pressed_bit) {
        if (held_test(input, code)) return SessionInputPostOk;
    } else if (!held_test(input, code)) {
        return SessionInputPostOk;
    }
    session_input_pending_t entry;
    memset(&entry, 0, sizeof entry);
    entry.kind = SessionInputPendingKey;
    entry.pressed = pressed_bit;
    entry.code = (uint16_t)code;
    entry.window = input->pair_window;
    entry.generation = input->pair_generation;
    /* A key never fails to enqueue: a full queue is resolved by release-all, which covers it. */
    if (enqueue(input, &entry) == EnqueueAppended) {
        if (pressed_bit) {
            held_set(input, code);
        } else {
            input->held[code >> 3] = (uint8_t)(input->held[code >> 3] & ~(1u << (code & 7u)));
            input->held_count--;
        }
    }
    uint64_t now_ns = 0;
    if (clock_ns(&now_ns) != 0) {
        input->terminal = 1;
        input->phase = SessionInputTerminal;
        return SessionInputPostTerminal;
    }
    if (waddle_keyboard_repeat_key(&input->repeat, (uint16_t)code, pressed_bit,
            waddle_keyboard_repeat_key_repeatable((uint16_t)code), now_ns) != WaddleKeyboardRepeatOk) {
        repeat_restart(input);
    }
    return SessionInputPostOk;
}

int session_input_post_modifiers(session_input_t *input, uint32_t depressed, uint32_t latched, uint32_t locked) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (!input->forwarding) return SessionInputPostOk;
    waddle_keyboard_lock_modifiers_t mods;
    mods.depressed = depressed;
    mods.latched = latched;
    mods.locked = locked;
    waddle_keyboard_lock_host_decision_t decision;
    memset(&decision, 0, sizeof decision);
    if (waddle_keyboard_lock_host_observe(&input->lock_host, &mods, &input->indices, &decision) != WaddleKeyboardLockOk) {
        input->terminal = 1;
        input->phase = SessionInputTerminal;
        return SessionInputPostTerminal;
    }
    if (!decision.send) return SessionInputPostOk;
    session_input_pending_t entry;
    memset(&entry, 0, sizeof entry);
    entry.kind = SessionInputPendingLocks;
    entry.locks = decision.mask;
    if (enqueue(input, &entry) == EnqueueFailed) return SessionInputPostTerminal;
    return SessionInputPostOk;
}

int session_input_post_keymap(session_input_t *input, const uint8_t *text, size_t length) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (text == NULL && length > 0) return SessionInputPostInvalidArgument;
    waddle_keymap_names_t names;
    memset(&names, 0, sizeof names);
    if (waddle_keymap_names_parse(text, length, &names) != WaddleKeymapNamesOk) {
        return stop_forwarding(input) == SessionInputPostOk ? SessionInputPostKeymapInvalid : SessionInputPostTerminal;
    }
    input->indices.caps = names.caps;
    input->indices.num = names.num;
    input->indices.scroll = names.scroll;
    if (names.has_layout) {
        waddle_keyboard_layouts_outcome_t outcome;
        memset(&outcome, 0, sizeof outcome);
        const uint8_t *variant = names.has_variant ? names.variant : NULL;
        size_t variant_length = names.has_variant ? names.variant_len : 0u;
        if (waddle_keyboard_layouts_observe(&input->layouts, names.layout, names.layout_len,
                variant, variant_length, &outcome) == WaddleKeyboardLayoutsOk &&
            outcome.kind == WaddleKeyboardLayoutsSend) {
            memcpy(input->klid, outcome.klid.bytes, WaddleKeyboardLayoutsKlidBytes);
            input->klid_known = 1;
            input->layout_dirty = 1;
        }
    }
    return SessionInputPostOk;
}

int session_input_post_repeat_info(session_input_t *input, int32_t rate, int32_t delay_ms) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (rate < 0 || delay_ms < 0) return SessionInputPostInvalidArgument;
    if (waddle_keyboard_repeat_configure(&input->repeat, rate > 0 ? 1u : 0u, rate, delay_ms) != WaddleKeyboardRepeatOk) {
        return SessionInputPostInvalidArgument;
    }
    input->repeat_rate = rate;
    input->repeat_delay_ms = delay_ms;
    input->repeat_known = 1;
    input->repeat_dirty = 1;
    return SessionInputPostOk;
}

int session_input_post_inhibit(session_input_t *input, const waddle_keyboard_inhibit_event_t *event) {
    int guard = post_guard(input);
    if (guard != SessionInputPostOk) return guard;
    if (event == NULL) return SessionInputPostInvalidArgument;
    if (event->kind == WaddleKeyboardInhibitEventFocusEnter || event->kind == WaddleKeyboardInhibitEventFocusLeave) {
        return SessionInputPostInvalidArgument;
    }
    return inhibit_apply(input, event);
}

/* ------------------------------------------------------------------------- */
/* Inbound frames                                                             */
/* ------------------------------------------------------------------------- */

/** @brief Accepts LeaseRevoked: epoch must be 1 first, then current+1 after Ready. Starts the barrier. */
static int handle_revoked(session_input_t *input, const uint8_t *payload, size_t length) {
    uint64_t epoch_value = 0;
    if (interop_field_uint(payload, length, InteropTagLeaseEpoch, 8u, &epoch_value) != InteropErrorNone) return -1;
    uint64_t expected = 0;
    if (input->phase == SessionInputAwaitInitialRevocation) {
        expected = 1u;
    } else if (input->phase == SessionInputReady) {
        if (input->epoch == UINT64_MAX) return -1;
        expected = input->epoch + 1u;
    } else {
        return -1;
    }
    if (epoch_value != expected) return -1;
    input->epoch = epoch_value;
    input->forwarding = 0;
    input->pending_count = 0;
    input->release_all_pending = 0;
    held_clear_all(input);
    repeat_stop(input);
    lock_stop(input);
    uint64_t now_ms = 0;
    if (clock_ms(&now_ms) != 0) return -1;
    input->handshake_deadline_ms = now_ms + (uint64_t)SessionInputHandshakeMs;
    input->phase = SessionInputBarrier;
    return send_ack(input) == TransmitFailed ? -1 : 0;
}

/** @brief Accepts LeaseReady: older epoch is a no-op; the matching epoch must be in AwaitReady with the ack serial. */
static int handle_ready(session_input_t *input, const uint8_t *payload, size_t length) {
    uint64_t serial_value = 0;
    uint64_t epoch_value = 0;
    if (interop_field_uint(payload, length, InteropTagSerial, 4u, &serial_value) != InteropErrorNone) return -1;
    if (interop_field_uint(payload, length, InteropTagLeaseEpoch, 8u, &epoch_value) != InteropErrorNone) return -1;
    if (epoch_value < input->epoch) return 0;
    if (epoch_value > input->epoch) return -1;
    if (input->phase != SessionInputAwaitReady) return -1;
    if (serial_value != input->ack_serial) return -1;
    uint64_t now_ms = 0;
    if (clock_ms(&now_ms) != 0 || now_ms >= input->handshake_deadline_ms) return -1;
    input->phase = SessionInputReady;
    input->handshake_deadline_ms = 0;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* session_handler_t callbacks                                                */
/* ------------------------------------------------------------------------- */

static int cb_owns_type(void *context, uint32_t message_type) {
    (void)context;
    return message_type == InteropMsgLeaseReady || message_type == InteropMsgLeaseRevoked ||
        message_type == InteropMsgCaretRect;
}

static int cb_on_ready(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender) {
    session_input_t *input = context;
    if (input->closed || input->terminal) return SessionInputCallbackFailed;
    input->input_enabled = (negotiated_capabilities & InteropCapInput) != 0;
    input->layout_enabled = (negotiated_capabilities & InteropCapKeyboardLayout) != 0;
    if (input->klid_known) input->layout_dirty = 1;
    if (input->repeat_known) input->repeat_dirty = 1;
    input->sender = sender;
    int rc = flush(input);
    input->sender = NULL;
    return rc;
}

static int cb_on_frame(void *context, uint32_t message_type, uint32_t request_id, const uint8_t *frame,
    size_t length, interop_host_feature_sender_t *sender) {
    (void)request_id;
    session_input_t *input = context;
    if (input->closed || input->terminal) return SessionInputCallbackFailed;
    if (length < InteropHeaderBytes) return SessionInputCallbackFailed;
    const uint8_t *payload = frame + InteropHeaderBytes;
    size_t payload_length = length - InteropHeaderBytes;
    input->sender = sender;
    int rc = 0;
    if (message_type == InteropMsgLeaseRevoked) {
        rc = handle_revoked(input, payload, payload_length);
    } else if (message_type == InteropMsgLeaseReady) {
        rc = handle_ready(input, payload, payload_length);
    } else if (message_type == InteropMsgCaretRect) {
        rc = 0;
    } else {
        rc = -1;
    }
    if (rc != 0) {
        input->terminal = 1;
        input->phase = SessionInputTerminal;
        input->sender = NULL;
        return SessionInputCallbackFailed;
    }
    rc = service(input);
    input->sender = NULL;
    return rc;
}

static int cb_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count) {
    session_input_t *input = context;
    if (input->terminal) return SessionInputCallbackFailed;
    if (input->display_fd < 0) {
        *count = 0;
        return 0;
    }
    if (capacity < 1u) return SessionInputCallbackFailed;
    fds[0].fd = input->display_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    *count = 1u;
    return 0;
}

static int cb_on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
    interop_host_feature_sender_t *sender) {
    (void)now_ms;
    session_input_t *input = context;
    if (input->closed || input->terminal) return SessionInputCallbackFailed;
    input->sender = sender;
    int rc = 0;
    if (count > 0u && fds[0].revents != 0 && input->dispatch != NULL) {
        rc = input->dispatch(input->dispatch_context);
        if (rc != 0) {
            input->terminal = 1;
            input->phase = SessionInputTerminal;
            input->sender = NULL;
            return SessionInputCallbackFailed;
        }
    }
    rc = service(input);
    input->sender = NULL;
    return rc;
}

static int cb_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender) {
    (void)now_ms;
    session_input_t *input = context;
    if (input->closed || input->terminal) return SessionInputCallbackFailed;
    input->sender = sender;
    int rc = service(input);
    input->sender = NULL;
    return rc;
}

static uint64_t cb_next_deadline_ms(void *context, uint64_t now_ms) {
    (void)now_ms;
    session_input_t *input = context;
    if (input->closed || input->terminal) return UINT64_MAX;
    uint64_t deadline = UINT64_MAX;
    if (input->phase == SessionInputBarrier || input->phase == SessionInputAwaitReady) {
        deadline = input->handshake_deadline_ms;
    }
    uint8_t present = 0;
    uint64_t repeat_deadline_ns = 0;
    if (waddle_keyboard_repeat_next_deadline(&input->repeat, &present, &repeat_deadline_ns) == WaddleKeyboardRepeatOk &&
        present) {
        uint64_t repeat_ms = deadline_ms_ceil(repeat_deadline_ns);
        if (repeat_ms < deadline) deadline = repeat_ms;
    }
    return deadline;
}

/** @brief Final notification. Exact-once: later calls return at once. Sends nothing. */
static void cb_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender) {
    (void)reason;
    (void)sender;
    session_input_t *input = context;
    if (input->closed) return;
    input->closed = 1;
    input->sender = NULL;
    if (input->focused) {
        input->forwarding = 0;
        input->focused = 0;
        (void)inhibit_focus(input, WaddleKeyboardInhibitEventFocusLeave, input->focus_window, input->focus_generation);
    }
    input->pending_count = 0;
    input->release_all_pending = 0;
    held_clear_all(input);
    repeat_stop(input);
    lock_stop(input);
}

session_handler_t session_input_make_handler(session_input_t *input) {
    session_handler_t handler;
    memset(&handler, 0, sizeof handler);
    handler.name = "session_input";
    handler.context = input;
    handler.capabilities = InteropCapInput | InteropCapKeyboardLayout;
    handler.owns_type = cb_owns_type;
    handler.on_ready = cb_on_ready;
    handler.on_frame = cb_on_frame;
    handler.poll_fds = cb_poll_fds;
    handler.on_fds = cb_on_fds;
    handler.on_tick = cb_on_tick;
    handler.next_deadline_ms = cb_next_deadline_ms;
    handler.on_closing = cb_on_closing;
    return handler;
}
