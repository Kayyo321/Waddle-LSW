/**
 * @file session_windows_handler.c
 * @brief Windows feature handler of the waddle session (see session_windows_handler.h for the limits).
 *
 * Layering: frames are decoded and encoded with the codec C API; the connection is host_wayland_core, the window
 * realization host_wayland_windows, and the per-window state machine window_state (C ABI). This file owns the
 * reducer table, the pending outbound list and the policy of 4.3, 4.5 and 4.7.
 */
#define _POSIX_C_SOURCE 200809L

#include "session_windows_handler.h"

#include "host_popup_positioner.h"
#include "waddle/interop_protocol.h"

#include <string.h>
#include <time.h>

/** @brief Re-check period for pending outbound frames while any remain (the runtime ticks at least every 100 ms). */
#define SessionWindowsPendingPeriodMs 100u

/* ---- small helpers ----------------------------------------------------------------------------------- */

static uint64_t now_of(const session_windows_handler_t *handler) {
    if (handler->config.now_ms != NULL) return handler->config.now_ms();
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

/** @brief Payload of a complete frame, after its header. Returns 0 when the frame is short or malformed. */
static int frame_payload(const uint8_t *frame, size_t length, const uint8_t **payload, size_t *payload_length) {
    interop_header_t header;
    if (frame == NULL || interop_header_decode(frame, length, &header) != InteropErrorNone) return 0;
    if (length < InteropHeaderBytes || (size_t)header.length > length - InteropHeaderBytes) return 0;
    *payload = frame + InteropHeaderBytes;
    *payload_length = header.length;
    return 1;
}

static int read_u64(const uint8_t *payload, size_t length, uint16_t tag, uint64_t *value) {
    return interop_field_uint(payload, length, tag, 8u, value) == 0;
}

static int read_u32(const uint8_t *payload, size_t length, uint16_t tag, uint32_t *value) {
    uint64_t wide = 0;
    if (interop_field_uint(payload, length, tag, 4u, &wide) != 0) return 0;
    *value = (uint32_t)wide;
    return 1;
}

static int read_u8(const uint8_t *payload, size_t length, uint16_t tag, uint32_t *value) {
    uint64_t wide = 0;
    if (interop_field_uint(payload, length, tag, 1u, &wide) != 0) return 0;
    *value = (uint32_t)wide;
    return 1;
}

/** @brief Window pair of a frame whose payload begins with the Window and Generation fields. */
static int read_identity(const uint8_t *frame, size_t length, uint64_t *window, uint64_t *generation) {
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!frame_payload(frame, length, &payload, &payload_length)) return 0;
    return read_u64(payload, payload_length, InteropTagWindow, window) && read_u64(payload, payload_length, InteropTagGeneration, generation);
}

/* ---- outbound frames (codec writer, host direction, request identifier zero) -------------------------- */

static size_t encode_identity_frame(uint8_t *buffer, size_t capacity, uint32_t type, uint64_t window, uint64_t generation, const int32_t *rect) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    if (rect != NULL) {
        interop_put_rect(&writer, InteropTagRect, rect[0], rect[1], (uint32_t)rect[2], (uint32_t)rect[3]);
    }
    size_t length = 0;
    if (interop_writer_finish(&writer, (uint16_t)type, 0u, InteropFromHost, &length) != InteropErrorNone) return 0;
    return length;
}

static size_t encode_configure(uint8_t *buffer, size_t capacity, uint64_t window, uint64_t generation, uint32_t state, const int32_t *rect) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    if (rect != NULL) interop_put_rect(&writer, InteropTagRect, rect[0], rect[1], (uint32_t)rect[2], (uint32_t)rect[3]);
    interop_put_uint(&writer, InteropTagState, state);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgWindowConfigure, 0u, InteropFromHost, &length) != InteropErrorNone) return 0;
    return length;
}

static size_t encode_frame_release(uint8_t *buffer, size_t capacity, uint64_t window, uint64_t generation, uint32_t slot, uint64_t sequence) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagSlot, slot);
    interop_put_uint(&writer, InteropTagFrameSequence, sequence);
    size_t length = 0;
    if (interop_writer_finish(&writer, InteropMsgFrameRelease, 0u, InteropFromHost, &length) != InteropErrorNone) return 0;
    return length;
}

/* ---- sending: feature sender, pending list, flush ---------------------------------------------------- */

/** @brief Send one frame through the running callback's sender, with the result the runtime reported. */
static int send_now(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    return interop_host_feature_send(handler->sender, frame, length);
}

/** @brief Store a frame in the pending list. A WindowConfigure replaces the newest pending one for the same window. */
/** @brief Record a fatal condition: every later callback of this handler returns nonzero. Returns 1. */
static int fail(session_windows_handler_t *handler) {
    handler->failed = 1;
    return 1;
}

static int enqueue(session_windows_handler_t *handler, uint32_t type, const uint8_t *frame, size_t length, uint64_t window, uint64_t generation) {
    if (type == InteropMsgWindowConfigure) {
        for (uint32_t index = 0; index < handler->pending_count; ++index) {
            if (handler->pending[index].type != InteropMsgWindowConfigure) continue;
            if (handler->pending[index].window != window || handler->pending[index].generation != generation) continue;
            memcpy(handler->pending[index].bytes, frame, length);
            handler->pending[index].length = length;
            ++handler->counters.configures_coalesced;
            return 0;
        }
    }
    if (handler->pending_count == SessionWindowsMaxPending) {
        ++handler->counters.pending_overflow;
        return fail(handler);
    }
    uint32_t slot = handler->pending_count++;
    memcpy(handler->pending[slot].bytes, frame, length);
    handler->pending[slot].length = length;
    handler->pending[slot].type = type;
    handler->pending[slot].window = window;
    handler->pending[slot].generation = generation;
    ++handler->counters.frames_queued;
    return 0;
}

/**
 * @brief Send or queue one outbound frame. Sends go out at once when a sender is running and nothing is queued
 * (FIFO order is kept). QueueFull queues the frame; Draining drops it (the session is closing); any other result
 * fails the run.
 */
static int emit(session_windows_handler_t *handler, uint32_t type, const uint8_t *frame, size_t length, uint64_t window, uint64_t generation) {
    if (handler->sender != NULL && handler->pending_count == 0) {
        int result = send_now(handler, frame, length);
        if (result == HostFeatureSuccess) {
            ++handler->counters.frames_sent;
            return 0;
        }
        if (result == HostFeatureDraining) {
            ++handler->counters.frames_dropped_draining;
            return 0;
        }
        if (result != HostFeatureQueueFull) return fail(handler);
    }
    return enqueue(handler, type, frame, length, window, generation);
}

/** @brief Send the pending frames in order until the queue is full again. Returns nonzero only on a failed send. */
static int flush_pending(session_windows_handler_t *handler) {
    if (handler->sender == NULL) return 0;
    while (handler->pending_count > 0) {
        int result = send_now(handler, handler->pending[0].bytes, handler->pending[0].length);
        if (result == HostFeatureSuccess) {
            ++handler->counters.frames_sent;
            memmove(&handler->pending[0], &handler->pending[1], sizeof handler->pending[0] * (handler->pending_count - 1));
            --handler->pending_count;
            continue;
        }
        if (result == HostFeatureQueueFull) return 0;
        if (result == HostFeatureDraining) {
            handler->counters.frames_dropped_draining += handler->pending_count;
            handler->pending_count = 0;
            return 0;
        }
        return fail(handler);
    }
    return 0;
}

/**
 * @brief Drop the pending window frames whose window pair is no longer realized (4.3). A WindowConfigure, WindowClose,
 * PopupRepositioned or PopupDismissed names a window; once the windows layer retired that window (by a guest retire, a
 * failed delivery or a reset), sending the frame would tell the guest about a window it no longer has. FrameRelease is
 * not window-scoped in this sense and is kept. The list keeps its order. Called after every callback that can retire a
 * window and before that callback flushes.
 */
static void purge_retired(session_windows_handler_t *handler) {
    uint32_t kept = 0;
    for (uint32_t index = 0; index < handler->pending_count; ++index) {
        uint32_t type = handler->pending[index].type;
        int window_scoped = type == InteropMsgWindowConfigure || type == InteropMsgWindowClose || type == InteropMsgPopupRepositioned || type == InteropMsgPopupDismissed;
        if (window_scoped && !host_wayland_windows_contains(&handler->windows, handler->pending[index].window, handler->pending[index].generation)) {
            ++handler->counters.frames_dropped_retired;
            continue;
        }
        if (kept != index) handler->pending[kept] = handler->pending[index];
        ++kept;
    }
    handler->pending_count = kept;
}

/** @brief Flush the Wayland output buffer. WouldBlock is not an error; the poll requests POLLOUT instead. */
static int flush_core(session_windows_handler_t *handler) {
    if (!handler->wayland_ready) return 0;
    return host_wayland_core_flush(&handler->core) == HostWaylandCoreDisplayError ? fail(handler) : 0;
}

/* ---- window-state reducers ---------------------------------------------------------------------------- */

static int reducer_find(const session_windows_handler_t *handler, uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) {
        if (handler->reducers[index].used && handler->reducers[index].window == window && handler->reducers[index].generation == generation) return (int)index;
    }
    return -1;
}

static void reducer_drop(session_windows_handler_t *handler, uint32_t index) {
    waddle_window_state_destroy(&handler->reducers[index].handle);
    handler->reducers[index].used = 0;
}

/** @brief Create the reducer of a window pair. Returns the slot, or -1 when the pair exists or the state is refused. */
static int reducer_create(session_windows_handler_t *handler, uint64_t window, uint64_t generation, uint32_t state, uint8_t popup) {
    if (reducer_find(handler, window, generation) >= 0) return -1;
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) {
        if (handler->reducers[index].used) continue;
        waddle_window_state_handle_t *handle = NULL;
        if (waddle_window_state_create(window, (uint32_t)generation, state, &handle) != WaddleWindowStateOk) return -1;
        handler->reducers[index].used = 1;
        handler->reducers[index].window = window;
        handler->reducers[index].generation = generation;
        handler->reducers[index].handle = handle;
        handler->reducers[index].popup = popup;
        return (int)index;
    }
    return -1;
}

static waddle_window_state_status_t reducer_plan(session_windows_handler_t *handler, uint32_t index, const waddle_window_state_input_t *input, waddle_window_state_plan_t *plan) {
    return waddle_window_state_plan(handler->reducers[index].handle, handler->reducers[index].window, (uint32_t)handler->reducers[index].generation, input, plan);
}

static waddle_window_state_status_t reducer_complete(session_windows_handler_t *handler, uint32_t index, waddle_window_state_plan_t *plan, uint32_t succeeded, uint64_t completed) {
    return waddle_window_state_complete(handler->reducers[index].handle, plan, succeeded, completed);
}

/**
 * @brief Execute the host actions of a plan in order (4.5). Guest actions are the guest's to perform and count as
 * completed. Returns zero when every action succeeded; otherwise the completed prefix is in *completed.
 *
 * A popup has no maximized, minimized or fullscreen state (xdg_popup has no such requests), so for a popup role every
 * host action is satisfied without a request. Before this rule the windows layer refused each one, which was counted as
 * a failed host request and left the popup's reducer unsynchronized.
 */
static int apply_actions(session_windows_handler_t *handler, uint64_t window, uint64_t generation, uint8_t popup, const waddle_window_state_plan_t *plan, uint64_t *completed) {
    *completed = 0;
    if (popup) {
        *completed = plan->action_count;
        return 0;
    }
    for (uint64_t index = 0; index < plan->action_count && index < WaddleWindowStateMaxActions; ++index) {
        const waddle_window_state_action_t *action = &plan->actions[index];
        host_wayland_status_t status = HostWaylandOk;
        switch (action->kind) {
        case WaddleWindowStateActionHostSetMinimized: status = host_wayland_windows_set_minimized(&handler->windows, window, generation); break;
        case WaddleWindowStateActionHostSetMaximized: status = host_wayland_windows_set_maximized(&handler->windows, window, generation, true); break;
        case WaddleWindowStateActionHostUnsetMaximized: status = host_wayland_windows_set_maximized(&handler->windows, window, generation, false); break;
        case WaddleWindowStateActionHostSetFullscreen: status = host_wayland_windows_set_fullscreen(&handler->windows, window, generation, true); break;
        case WaddleWindowStateActionHostUnsetFullscreen: status = host_wayland_windows_set_fullscreen(&handler->windows, window, generation, false); break;
        default: break;
        }
        if (status != HostWaylandOk) return 1;
        *completed = index + 1;
    }
    *completed = plan->action_count;
    return 0;
}

/**
 * @brief Plan one input on a reducer, execute its host actions and complete it. A failed action completes the
 * prefix that succeeded, which makes the reducer resynchronize on the next observation (window_state contract).
 * Returns nonzero when the plan itself was refused; the refusal is counted by the caller.
 */
static int run_input(session_windows_handler_t *handler, uint32_t index, const waddle_window_state_input_t *input) {
    waddle_window_state_plan_t plan;
    if (reducer_plan(handler, index, input, &plan) != WaddleWindowStateOk) return 1;
    uint64_t completed = 0;
    int failed = apply_actions(handler, handler->reducers[index].window, handler->reducers[index].generation, handler->reducers[index].popup, &plan, &completed);
    if (failed) ++handler->counters.host_actions_failed;
    if (reducer_complete(handler, index, &plan, failed ? 0u : 1u, completed) != WaddleWindowStateOk) return 1;
    return 0;
}

static void observe_guest(session_windows_handler_t *handler, uint32_t index, uint32_t state) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof input);
    input.kind = WaddleWindowStateInputGuestObservation;
    input.state = state;
    if (run_input(handler, index, &input) == 0) ++handler->counters.guest_observations_applied;
}

/* ---- compositor events -------------------------------------------------------------------------------- */

/** @brief Window-state bits of a configure that the guest needs (Activated and the window state bits). */
static uint32_t configure_bits(uint32_t bits) {
    return bits & (WaddleWindowStateActivated | WaddleWindowStateWindowMask);
}

static void on_configure(void *user, uint64_t window, uint64_t generation, uint32_t bits, uint32_t width, uint32_t height) {
    session_windows_handler_t *handler = user;
    uint32_t state = configure_bits(bits);
    waddle_window_state_plan_t plan;
    memset(&plan, 0, sizeof plan);
    uint64_t physical_width = (uint64_t)width * handler->scale;
    uint64_t physical_height = (uint64_t)height * handler->scale;
    int have_rect = width != 0 && height != 0 && physical_width <= 16384u && physical_height <= 16384u;
    int32_t rect[4] = {0, 0, (int32_t)physical_width, (int32_t)physical_height};
    int index = reducer_find(handler, window, generation);
    waddle_window_state_input_t input;
    int planned = 0;
    if (index >= 0) {
        memset(&input, 0, sizeof input);
        input.kind = WaddleWindowStateInputHostConfigure;
        input.state = state;
        input.width = have_rect ? (uint32_t)physical_width : 0u;
        input.height = have_rect ? (uint32_t)physical_height : 0u;
        planned = reducer_plan(handler, (uint32_t)index, &input, &plan) == WaddleWindowStateOk;
    }
    uint8_t frame[SessionWindowsFrameBytes];
    size_t length = encode_configure(frame, sizeof frame, window, generation, state, have_rect ? rect : NULL);
    if (length == 0) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    if (emit(handler, InteropMsgWindowConfigure, frame, length, window, generation) != 0) return;
    ++handler->counters.configures_sent;
    if (planned) reducer_complete(handler, (uint32_t)index, &plan, 1u, plan.action_count);
}

static void on_close(void *user, uint64_t window, uint64_t generation) {
    session_windows_handler_t *handler = user;
    int index = reducer_find(handler, window, generation);
    if (index >= 0) {
        waddle_window_state_input_t input;
        memset(&input, 0, sizeof input);
        input.kind = WaddleWindowStateInputCloseRequest;
        waddle_window_state_plan_t plan;
        if (reducer_plan(handler, (uint32_t)index, &input, &plan) == WaddleWindowStateOk) {
            reducer_complete(handler, (uint32_t)index, &plan, 1u, plan.action_count);
        }
    }
    uint8_t frame[SessionWindowsFrameBytes];
    size_t length = encode_identity_frame(frame, sizeof frame, InteropMsgWindowClose, window, generation, NULL);
    if (length != 0 && emit(handler, InteropMsgWindowClose, frame, length, window, generation) == 0) ++handler->counters.closes_sent;
}

static void on_popup_repositioned(void *user, uint64_t window, uint64_t generation, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    session_windows_handler_t *handler = user;
    int32_t rect[4] = {x, y, (int32_t)width, (int32_t)height};
    uint8_t frame[SessionWindowsFrameBytes];
    size_t length = encode_identity_frame(frame, sizeof frame, InteropMsgPopupRepositioned, window, generation, rect);
    if (length != 0 && emit(handler, InteropMsgPopupRepositioned, frame, length, window, generation) == 0) ++handler->counters.popup_frames_sent;
}

static void on_popup_dismissed(void *user, uint64_t window, uint64_t generation) {
    session_windows_handler_t *handler = user;
    uint8_t frame[SessionWindowsFrameBytes];
    size_t length = encode_identity_frame(frame, sizeof frame, InteropMsgPopupDismissed, window, generation, NULL);
    if (length != 0 && emit(handler, InteropMsgPopupDismissed, frame, length, window, generation) == 0) ++handler->counters.popup_frames_sent;
}

static void on_error(void *user, uint16_t refused_type, uint32_t error_code, uint64_t window, uint64_t generation) {
    session_windows_handler_t *handler = user;
    (void)refused_type;
    (void)error_code;
    (void)window;
    (void)generation;
    ++handler->counters.errors_reported;
}

/* ---- inbound frames ----------------------------------------------------------------------------------- */

static void on_core_seat(void *user, struct wl_seat *seat, bool added) {
    session_windows_handler_t *handler = user;
    if (!added) host_wayland_windows_forget_seat(&handler->windows, seat);
    if (handler->seat_listener != NULL) handler->seat_listener(handler->seat_user, seat, added);
}

static int core_has_seat(const session_windows_handler_t *handler, const struct wl_seat *seat) {
    if (seat == NULL) return 0;
    for (uint32_t index = 0; index < HostWaylandCoreMaxSeats; ++index) {
        if (host_wayland_core_seat_at(&handler->core, index) == seat) return 1;
    }
    return 0;
}

/** @brief Popup roles (xdg_popup): menu, tooltip and the IME preedit window. Toplevel roles are the other three. */
static uint8_t role_is_popup(uint32_t role) {
    return role == InteropRoleMenu || role == InteropRoleTooltip || role == InteropRoleImePreedit;
}

static void on_announce(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    uint32_t state = 0;
    uint32_t role = 0;
    if (!read_identity(frame, length, &window, &generation) || !frame_payload(frame, length, &payload, &payload_length) ||
        !read_u32(payload, payload_length, InteropTagState, &state) || !read_u8(payload, payload_length, InteropTagRole, &role)) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    if (reducer_find(handler, window, generation) >= 0) {
        ++handler->counters.announces_refused;
        return;
    }
    int index = reducer_create(handler, window, generation, state, role_is_popup(role));
    if (index < 0) {
        ++handler->counters.announces_refused;
        return;
    }
    host_wayland_status_t status = host_wayland_windows_handle_frame(&handler->windows, frame, length, now_of(handler));
    if (status != HostWaylandOk || !host_wayland_windows_contains(&handler->windows, window, generation)) {
        reducer_drop(handler, (uint32_t)index);
        ++handler->counters.announces_refused;
        return;
    }
    ++handler->counters.announces_realized;
    observe_guest(handler, (uint32_t)index, state);
}

static void on_update(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    uint32_t state = 0;
    if (!read_identity(frame, length, &window, &generation) || !frame_payload(frame, length, &payload, &payload_length) || !read_u32(payload, payload_length, InteropTagState, &state)) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    if (host_wayland_windows_handle_frame(&handler->windows, frame, length, now_of(handler)) != HostWaylandOk) return;
    ++handler->counters.updates_applied;
    int index = reducer_find(handler, window, generation);
    if (index >= 0) observe_guest(handler, (uint32_t)index, state);
}

static void on_retire(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    if (host_wayland_windows_handle_frame(&handler->windows, frame, length, now_of(handler)) != HostWaylandOk) return;
    ++handler->counters.retires_applied;
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) {
        if (!handler->reducers[index].used) continue;
        if (host_wayland_windows_contains(&handler->windows, handler->reducers[index].window, handler->reducers[index].generation)) continue;
        waddle_window_state_input_t input;
        memset(&input, 0, sizeof input);
        input.kind = WaddleWindowStateInputDestroyObserved;
        waddle_window_state_plan_t plan;
        if (reducer_plan(handler, index, &input, &plan) == WaddleWindowStateOk) {
            reducer_complete(handler, index, &plan, 1u, plan.action_count);
        }
        reducer_drop(handler, index);
    }
}

/** @brief FrameReady: the frames layer is not built, so the slot is released at once (see the header limits). */
static int on_frame_ready(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    uint32_t slot = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!frame_payload(frame, length, &payload, &payload_length) || !read_u64(payload, payload_length, InteropTagWindow, &window) ||
        !read_u64(payload, payload_length, InteropTagGeneration, &generation) || !read_u32(payload, payload_length, InteropTagSlot, &slot) ||
        !read_u64(payload, payload_length, InteropTagFrameSequence, &sequence)) {
        ++handler->counters.frame_fields_refused;
        return 0;
    }
    uint8_t release[SessionWindowsFrameBytes];
    size_t release_length = encode_frame_release(release, sizeof release, window, generation, slot, sequence);
    if (release_length == 0) return fail(handler);
    ++handler->counters.frames_dropped_no_buffer_layer;
    return emit(handler, InteropMsgFrameRelease, release, release_length, window, generation);
}

/**
 * @brief Interactive move or resize from the guest (4.5). The echoed serial T must name a live press that is the
 * latest press of the same window and generation, held with the left button, of the current lease, on a registered
 * seat. Resize also needs the Resizable capability in the reducer's last observation and a valid edge.
 */
static void guest_press_request(session_windows_handler_t *handler, uint64_t window, uint64_t generation, uint32_t serial, int resize, uint32_t edge) {
    host_wayland_press_info_t info;
    memset(&info, 0, sizeof info);
    int ignored = 1;
    if (host_wayland_windows_lookup_press(&handler->windows, serial, now_of(handler), &info) == HostWaylandOk &&
        info.window == window && info.generation == generation &&
        info.kind == HostWaylandPressPointer && info.button == SessionWindowsButtonLeft &&
        info.latest_for_window && info.lease_epoch == handler->lease_epoch && core_has_seat(handler, info.seat)) {
        int index = reducer_find(handler, window, generation);
        int allowed = 1;
        if (resize) {
            waddle_window_state_snapshot_t snapshot;
            allowed = index >= 0 && waddle_window_state_snapshot(handler->reducers[index].handle, &snapshot) == WaddleWindowStateOk &&
                (snapshot.guest_capabilities & WaddleWindowStateResizable) != 0;
        }
        if (allowed) {
            host_wayland_status_t status = resize
                ? host_wayland_windows_begin_resize(&handler->windows, window, generation, info.seat, info.wayland_serial, edge)
                : host_wayland_windows_begin_move(&handler->windows, window, generation, info.seat, info.wayland_serial);
            ignored = status != HostWaylandOk;
        }
    }
    if (resize) {
        if (ignored) ++handler->counters.resizes_ignored; else ++handler->counters.resizes_applied;
    } else {
        if (ignored) ++handler->counters.moves_ignored; else ++handler->counters.moves_applied;
    }
}

static void on_move_request(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    uint32_t serial = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!read_identity(frame, length, &window, &generation) || !frame_payload(frame, length, &payload, &payload_length) || !read_u32(payload, payload_length, InteropTagSerial, &serial)) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    guest_press_request(handler, window, generation, serial, 0, 0);
}

static void on_resize_request(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    uint32_t serial = 0;
    uint32_t edge = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!read_identity(frame, length, &window, &generation) || !frame_payload(frame, length, &payload, &payload_length) ||
        !read_u32(payload, payload_length, InteropTagSerial, &serial) || !read_u8(payload, payload_length, InteropTagEdge, &edge)) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    guest_press_request(handler, window, generation, serial, 1, edge);
}

/** @brief Caption double click: the guest asks for the Maximized bit to flip; the reducer decides (Maximizable). */
static void on_state_request(session_windows_handler_t *handler, const uint8_t *frame, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    uint32_t state = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;
    if (!read_identity(frame, length, &window, &generation) || !frame_payload(frame, length, &payload, &payload_length) || !read_u32(payload, payload_length, InteropTagState, &state)) {
        ++handler->counters.frame_fields_refused;
        return;
    }
    int index = reducer_find(handler, window, generation);
    if (index < 0) {
        ++handler->counters.state_requests_ignored;
        return;
    }
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof input);
    input.kind = WaddleWindowStateInputHostStateRequest;
    input.maximize = (state & WaddleWindowStateMaximized) != 0 ? 1u : 0u;
    if (run_input(handler, (uint32_t)index, &input) == 0) ++handler->counters.state_requests_applied;
    else ++handler->counters.state_requests_ignored;
}

/* ---- session_handler_t callbacks ---------------------------------------------------------------------- */

static int owns_type(void *context, uint32_t message_type) {
    (void)context;
    switch (message_type) {
    case InteropMsgWindowAnnounce:
    case InteropMsgWindowUpdate:
    case InteropMsgWindowRetire:
    case InteropMsgFrameReady:
    case InteropMsgWindowMoveRequest:
    case InteropMsgWindowResizeRequest:
    case InteropMsgWindowStateRequest:
    case InteropMsgWindowIconChanged:
        return 1;
    default:
        return 0;
    }
}

static int on_ready(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender) {
    session_windows_handler_t *handler = context;
    if ((negotiated_capabilities & InteropCapWindows) == 0 || handler->wayland_ready) return 0;
    host_wayland_core_config_t core_config;
    core_config.display_name = handler->config.display_name;
    core_config.display = handler->config.display;
    if (host_wayland_core_init(&handler->core, &core_config) != HostWaylandCoreOk) return 1;
    const host_wayland_core_globals_t *globals = host_wayland_core_globals(&handler->core);
    host_wayland_config_t windows_config;
    memset(&windows_config, 0, sizeof windows_config);
    windows_config.compositor = globals->compositor;
    windows_config.wm_base = globals->wm_base;
    windows_config.wm_base_version = globals->wm_base_version;
    windows_config.decoration_manager = globals->decoration_manager;
    windows_config.device = handler->config.device;
    windows_config.scale_numerator = handler->config.scale_numerator;
    windows_config.scale_denominator = handler->config.scale_denominator;
    windows_config.callbacks.user = handler;
    windows_config.callbacks.on_configure = on_configure;
    windows_config.callbacks.on_close = on_close;
    windows_config.callbacks.on_popup_repositioned = on_popup_repositioned;
    windows_config.callbacks.on_popup_dismissed = on_popup_dismissed;
    windows_config.callbacks.on_error = on_error;
    if (host_wayland_windows_init(&handler->windows, &windows_config) != HostWaylandOk) {
        host_wayland_core_close(&handler->core);
        return 1;
    }
    host_wayland_core_set_seat_listener(&handler->core, on_core_seat, handler);
    handler->wayland_ready = 1;
    handler->sender = sender;
    int result = flush_pending(handler) || flush_core(handler);
    handler->sender = NULL;
    return handler->failed ? 1 : result;
}

static int on_frame(void *context, uint32_t message_type, uint32_t request_id, const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender) {
    session_windows_handler_t *handler = context;
    if (!handler->wayland_ready || handler->failed || request_id != 0) return 1;
    handler->sender = sender;
    ++handler->counters.frames_received;
    int result = flush_pending(handler);
    if (result == 0) {
        switch (message_type) {
        case InteropMsgWindowAnnounce: on_announce(handler, frame, length); break;
        case InteropMsgWindowUpdate: on_update(handler, frame, length); break;
        case InteropMsgWindowRetire: on_retire(handler, frame, length); break;
        case InteropMsgFrameReady: result = on_frame_ready(handler, frame, length); break;
        case InteropMsgWindowMoveRequest: on_move_request(handler, frame, length); break;
        case InteropMsgWindowResizeRequest: on_resize_request(handler, frame, length); break;
        case InteropMsgWindowStateRequest: on_state_request(handler, frame, length); break;
        case InteropMsgWindowIconChanged: ++handler->counters.icons_ignored; break;
        default: break;
        }
    }
    purge_retired(handler);
    if (result == 0) result = flush_pending(handler) || flush_core(handler);
    handler->sender = NULL;
    return handler->failed ? 1 : result;
}

static int poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count) {
    session_windows_handler_t *handler = context;
    *count = 0;
    if (!handler->wayland_ready || capacity == 0) return 0;
    fds[0].fd = host_wayland_core_fd(&handler->core);
    fds[0].events = host_wayland_core_poll_events(&handler->core);
    fds[0].revents = 0;
    *count = 1;
    return fds[0].fd < 0 ? 1 : 0;
}

static int on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms, interop_host_feature_sender_t *sender) {
    (void)now_ms;
    session_windows_handler_t *handler = context;
    if (!handler->wayland_ready) return 0;
    handler->sender = sender;
    int result = 0;
    if (count > 0) {
        host_wayland_core_status_t status = host_wayland_core_dispatch(&handler->core, fds[0].revents);
        if (status != HostWaylandCoreOk && status != HostWaylandCoreWouldBlock) result = fail(handler);
    }
    purge_retired(handler);
    if (result == 0) result = flush_pending(handler) || flush_core(handler);
    handler->sender = NULL;
    return handler->failed ? 1 : result;
}

static int on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender) {
    (void)now_ms;
    session_windows_handler_t *handler = context;
    if (!handler->wayland_ready) return 0;
    handler->sender = sender;
    int result = flush_pending(handler) || flush_core(handler);
    handler->sender = NULL;
    return handler->failed ? 1 : result;
}

static uint64_t next_deadline_ms(void *context, uint64_t now_ms) {
    const session_windows_handler_t *handler = context;
    if (!handler->wayland_ready || handler->pending_count == 0) return UINT64_MAX;
    return now_ms + SessionWindowsPendingPeriodMs;
}

static void on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender) {
    (void)reason;
    session_windows_handler_t *handler = context;
    handler->sender = sender;
    handler->closing = 1;
    handler->counters.frames_dropped_draining += handler->pending_count;
    handler->pending_count = 0;
    handler->sender = NULL;
    if (handler->wayland_ready) {
        host_wayland_windows_reset(&handler->windows);
        host_wayland_core_close(&handler->core);
        handler->wayland_ready = 0;
    }
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) {
        if (handler->reducers[index].used) reducer_drop(handler, index);
    }
}

/* ---- public functions ---------------------------------------------------------------------------------- */

int session_windows_handler_init(session_windows_handler_t *handler, const session_windows_config_t *config) {
    if (handler == NULL || config == NULL || config->device == NULL) return 1;
    size_t device_length = strnlen(config->device, 65);
    if (device_length == 0 || device_length > 64) return 1;
    uint32_t numerator = config->scale_numerator == 0 && config->scale_denominator == 0 ? 1u : config->scale_numerator;
    uint32_t denominator = config->scale_numerator == 0 && config->scale_denominator == 0 ? 1u : config->scale_denominator;
    uint32_t scale = 0;
    if (host_popup_positioner_integer_scale(numerator, denominator, &scale) != HostPopupOk) return 1;
    memset(handler, 0, sizeof *handler);
    handler->config = *config;
    handler->config.scale_numerator = numerator;
    handler->config.scale_denominator = denominator;
    handler->scale = scale;
    handler->initialized = 1;
    return 0;
}

session_handler_t session_windows_handler_descriptor(session_windows_handler_t *handler) {
    session_handler_t table;
    memset(&table, 0, sizeof table);
    table.name = "session_windows";
    table.context = handler;
    table.capabilities = InteropCapWindows;
    table.owns_type = owns_type;
    table.on_ready = on_ready;
    table.on_frame = on_frame;
    table.poll_fds = poll_fds;
    table.on_fds = on_fds;
    table.on_tick = on_tick;
    table.next_deadline_ms = next_deadline_ms;
    table.on_closing = on_closing;
    return table;
}

void session_windows_handler_counters(const session_windows_handler_t *handler, session_windows_counters_t *out) {
    if (out == NULL) return;
    if (handler == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = handler->counters;
}

void session_windows_set_lease_epoch(session_windows_handler_t *handler, uint64_t lease_epoch) {
    if (handler == NULL || !handler->initialized) return;
    handler->lease_epoch = lease_epoch;
    if (handler->wayland_ready) host_wayland_windows_drop_presses(&handler->windows, lease_epoch);
}

host_wayland_status_t session_windows_record_press(session_windows_handler_t *handler, const host_wayland_press_input_t *input) {
    if (handler == NULL || !handler->wayland_ready) return HostWaylandNotInitialized;
    return host_wayland_windows_record_press(&handler->windows, input);
}

void session_windows_release_press(session_windows_handler_t *handler, uint32_t protocol_serial) {
    if (handler == NULL || !handler->wayland_ready) return;
    host_wayland_windows_release_press(&handler->windows, protocol_serial);
}

void session_windows_set_seat_listener(session_windows_handler_t *handler, host_wayland_core_seat_listener_t listener, void *user) {
    if (handler == NULL) return;
    handler->seat_listener = listener;
    handler->seat_user = user;
}
