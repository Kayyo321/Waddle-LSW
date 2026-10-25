/**
 * @file session_app.c
 * @brief Composes feature handlers into one control-runtime feature ops table.
 *
 * Callback status: every handler callback returns zero, SessionHandlerRequestStop (a graceful
 * stop) or another nonzero value (a failure). The fan-out stops at the first nonzero status and
 * reports it to the runtime unchanged in kind: a stop becomes InteropHostFeatureStopRequested and
 * any other value becomes SessionAppFailure. on_closing reaches every handler exactly once in
 * reverse registration order, including handlers that never saw on_ready.
 */
#include "session_app.h"

#include <stdbool.h>
#include <string.h>

/** @brief Runtime status for a handler failure; distinct from the stop value. */
enum { SessionAppFailure = 2 };

/** @brief Per-run routing state; lives on the stack of session_app_run_detailed. */
typedef struct session_app_state_t {
    const session_handler_t *handlers; /**< Borrowed table in registration order. */
    size_t count; /**< Number of handlers, 1..SessionAppMaxHandlers. */
    size_t slice_offset[SessionAppMaxHandlers]; /**< First runtime descriptor of each handler's slice. */
    size_t slice_count[SessionAppMaxHandlers]; /**< Descriptors reported by each handler in the last poll. */
    size_t descriptor_total; /**< Sum of slice_count after the last poll. */
} session_app_state_t;

/** @brief Translate one handler status into the runtime's status space. */
static int session_app_translate(int status)
{
    if (status == 0) return 0;
    if (status == SessionHandlerRequestStop) return InteropHostFeatureStopRequested;
    return SessionAppFailure;
}

static int session_app_on_ready(void *context, uint64_t negotiated,
    interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t index;

    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        int status;

        if (handler->on_ready == NULL) continue;
        status = session_app_translate(handler->on_ready(handler->context, negotiated, sender));
        if (status != 0) return status;
    }
    return 0;
}

static int session_app_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t owners = 0;
    size_t owner = 0;
    size_t index;

    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        if (handler->owns_type(handler->context, message_type) != 0) {
            ++owners;
            owner = index;
        }
    }
    if (owners != 1) return SessionAppFailure;
    if (state->handlers[owner].on_frame == NULL) return SessionAppFailure;
    return session_app_translate(state->handlers[owner].on_frame(
        state->handlers[owner].context, message_type, request_id, frame, length, sender));
}

static int session_app_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    session_app_state_t *state = context;
    size_t total = 0;
    size_t index;

    (void)capacity; /* The runtime always offers InteropHostFeatureFdCapacity entries. */
    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        struct pollfd local[SessionHandlerMaxDescriptors];
        size_t reported = 0;
        size_t position;

        memset(local, 0, sizeof(local));
        state->slice_offset[index] = total;
        state->slice_count[index] = 0;
        if (handler->poll_fds == NULL) continue;
        if (handler->poll_fds(handler->context, local, SessionHandlerMaxDescriptors, &reported) != 0)
            return SessionAppFailure;
        if (reported > SessionHandlerMaxDescriptors) return SessionAppFailure;
        if (reported > InteropHostFeatureFdCapacity - total) return SessionAppFailure;
        for (position = 0; position < reported; ++position) {
            fds[total + position] = local[position];
            fds[total + position].revents = 0;
        }
        state->slice_count[index] = reported;
        total += reported;
    }
    state->descriptor_total = total;
    *count = total;
    return 0;
}

static int session_app_on_fds(void *context, const struct pollfd *fds, size_t count,
    uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t index;

    (void)count; /* The runtime passes back exactly the descriptors poll_fds reported. */
    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        bool ready = false;
        size_t position;
        int status;

        for (position = 0; position < state->slice_count[index]; ++position) {
            if (fds[state->slice_offset[index] + position].revents != 0) ready = true;
        }
        if (!ready) continue;
        status = session_app_translate(handler->on_fds(handler->context,
            fds + state->slice_offset[index], state->slice_count[index], now_ms, sender));
        if (status != 0) return status;
    }
    return 0;
}

static int session_app_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t index;

    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        int status;

        if (handler->on_tick == NULL) continue;
        status = session_app_translate(handler->on_tick(handler->context, now_ms, sender));
        if (status != 0) return status;
    }
    return 0;
}

static int session_app_on_peer_error(void *context, uint32_t error_code, uint32_t request_id,
    interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t index;

    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        int status;

        if (handler->on_error == NULL) continue;
        status = session_app_translate(handler->on_error(handler->context, error_code,
            request_id, sender));
        if (status != 0) return status;
    }
    return 0;
}

static uint64_t session_app_next_deadline(void *context, uint64_t now_ms)
{
    session_app_state_t *state = context;
    uint64_t earliest = UINT64_MAX;
    size_t index;

    for (index = 0; index < state->count; ++index) {
        const session_handler_t *handler = &state->handlers[index];
        uint64_t deadline;

        if (handler->next_deadline_ms == NULL) continue;
        deadline = handler->next_deadline_ms(handler->context, now_ms);
        if (deadline < earliest) earliest = deadline;
    }
    return earliest;
}

static void session_app_on_closing(void *context, uint32_t reason,
    interop_host_feature_sender_t *sender)
{
    session_app_state_t *state = context;
    size_t index = state->count;

    /* The runtime calls this exactly once; every handler then gets exactly one closing call. */
    while (index-- > 0) {
        const session_handler_t *handler = &state->handlers[index];
        if (handler->on_closing != NULL)
            handler->on_closing(handler->context, reason, sender);
    }
}

/** @brief Validate handler registration; returns false for any rejected table. */
static bool session_app_validate(const session_app_config_t *config, uint64_t *union_out)
{
    uint64_t union_caps = 0;
    size_t index;
    size_t other;

    if (config->control.feature_ops != NULL) return false;
    if (config->control.admission == NULL || config->control.admission->local_capabilities != 0)
        return false;
    if (config->handler_count == 0) {
        *union_out = 0;
        return config->handlers == NULL;
    }
    if (config->handlers == NULL || config->handler_count > SessionAppMaxHandlers) return false;
    for (index = 0; index < config->handler_count; ++index) {
        const session_handler_t *handler = &config->handlers[index];
        if (handler->name == NULL || handler->owns_type == NULL) return false;
        if ((handler->capabilities & ~InteropCapabilityMask) != 0) return false;
        if (handler->poll_fds != NULL && handler->on_fds == NULL) return false;
        for (other = 0; other < index; ++other) {
            if (strcmp(config->handlers[other].name, handler->name) == 0) return false;
        }
        union_caps |= handler->capabilities;
    }
    *union_out = union_caps;
    return true;
}

interop_host_control_result_t session_app_run_detailed(
    const session_app_config_t *config,
    interop_host_control_owners_t *out_owners,
    interop_host_control_detail_t *detail)
{
    session_app_state_t state;
    interop_host_feature_ops_t ops;
    interop_host_session_config_t admission;
    interop_host_control_config_t control;
    uint64_t union_caps = 0;

    if (config == NULL || !session_app_validate(config, &union_caps))
        return HostControlInvalidArgument;
    if (config->handler_count == 0) {
        return interop_host_control_run_detailed(&config->control, out_owners, detail);
    }

    memset(&state, 0, sizeof(state));
    state.handlers = config->handlers;
    state.count = config->handler_count;
    memset(&ops, 0, sizeof(ops));
    ops.context = &state;
    ops.local_capabilities = union_caps;
    ops.on_ready = session_app_on_ready;
    ops.on_frame = session_app_on_frame;
    ops.poll_fds = session_app_poll_fds;
    ops.on_fds = session_app_on_fds;
    ops.on_tick = session_app_on_tick;
    ops.next_deadline_ms = session_app_next_deadline;
    ops.on_peer_error = session_app_on_peer_error;
    ops.on_closing = session_app_on_closing;

    /* The runtime requires admission and offer to match; the caller's zero offer is replaced here. */
    admission = *config->control.admission;
    admission.local_capabilities = union_caps;
    control = config->control;
    control.admission = &admission;
    control.feature_ops = &ops;
    return interop_host_control_run_detailed(&control, out_owners, detail);
}

interop_host_control_result_t session_app_run(
    const session_app_config_t *config,
    interop_host_control_owners_t *out_owners)
{
    return session_app_run_detailed(config, out_owners, NULL);
}
