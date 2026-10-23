#include "av_lease.h"
#include <string.h>
static int guest_fail(av_lease_guest_t *lease) {
    lease->phase = AvLeaseGuestTerminal; lease->deadline = 0; return -1;
}
static int host_fail(av_lease_host_t *lease) {
    lease->phase = AvLeaseHostTerminal; lease->deadline = 0; return -1;
}
int av_lease_timeout(uint64_t deadline, uint64_t now_ms, int maximum) {
    if (!now_ms || maximum < 0 || (deadline && now_ms >= deadline)) return -1;
    if (!deadline || deadline - now_ms > (uint64_t)maximum) return maximum;
    return (int)(deadline - now_ms);
}
int av_lease_guest_check(av_lease_guest_t *lease, uint64_t now_ms) {
    return lease->phase == AvLeaseGuestTerminal || av_lease_timeout(lease->deadline, now_ms, 0) < 0
        ? guest_fail(lease) : 0;
}
int av_lease_host_check(av_lease_host_t *lease, uint64_t now_ms) {
    return lease->phase == AvLeaseHostTerminal || av_lease_timeout(lease->deadline, now_ms, 0) < 0
        ? host_fail(lease) : 0;
}
static int revoke(av_lease_guest_t *lease, const av_lease_ops_t *ops, void *context) {
    if (lease->epoch == UINT64_MAX) return guest_fail(lease);
    ++lease->epoch; /* Reserved once, even if publication subsequently fails. */
    av_message_t message = {.type = MsgInputEpochRevoked, .lease_generation = lease->epoch};
    uint64_t now = ops->now_ms(context);
    if (!now || now > UINT64_MAX - AvLeaseTimeoutMs) return guest_fail(lease);
    lease->phase = AvLeaseAwaitAck;
    if (ops->publish(context, &message) != 0) return guest_fail(lease);
    now = ops->now_ms(context); /* Deadline starts at successful enqueue. */
    if (!now || now > UINT64_MAX - AvLeaseTimeoutMs) return guest_fail(lease);
    lease->deadline = now + AvLeaseTimeoutMs;
    return 0;
}
int av_lease_guest_begin(av_lease_guest_t *lease, const av_lease_ops_t *ops, void *context) {
    if (lease->phase != AvLeaseInitial || lease->epoch) return guest_fail(lease);
    return revoke(lease, ops, context);
}
int av_lease_guest_transition(av_lease_guest_t *lease, uint64_t window_id,
    uint64_t incarnation, const av_lease_ops_t *ops, void *context) {
    if (av_lease_guest_check(lease, ops->now_ms(context)) != 0) return -1;
    if (!lease->ever_anchored) return 0; /* Startup observation grants nothing. */
    if (!window_id || !incarnation) return guest_fail(lease);
    if (lease->anchor_id == window_id && lease->anchor_incarnation == incarnation) return 0;
    if (lease->phase != AvLeaseActive && lease->phase != AvLeaseUnfocusedReady) return guest_fail(lease);
    /* Suspend before mandatory release. The adapter never recursively reconciles. */
    lease->phase = AvLeaseUnfocusedReady;
    if (av_input_reset(&lease->input, &ops->input, context) != 0) return guest_fail(lease);
    if (ops->input.target(context, window_id, incarnation, 0) != 0) return guest_fail(lease);
    lease->anchor_id = window_id; lease->anchor_incarnation = incarnation;
    return revoke(lease, ops, context);
}
int av_lease_guest_apply(av_lease_guest_t *lease, const av_message_t *message,
    const av_lease_ops_t *ops, void *context) {
    uint8_t bytes[AvControlBytes];
    if (lease->phase == AvLeaseGuestTerminal) return -1;
    if (!message || (message->type != MsgInputEpochAck &&
        (message->type < MsgInputFocusV3 || message->type > MsgInputReleaseV3)) ||
        av_control_encode(message, bytes, sizeof(bytes)) != 0 ||
        message->buffer_index <= lease->input.serial) return guest_fail(lease);
    lease->input.serial = message->buffer_index;
    if (message->lease_generation < lease->epoch) return 0;
    if (message->lease_generation > lease->epoch || lease->phase == AvLeaseInitial)
        return guest_fail(lease);
    if (av_lease_guest_check(lease, ops->now_ms(context)) != 0) return -1;
    if (message->type == MsgInputEpochAck) {
        if (lease->phase != AvLeaseAwaitAck || ops->reconcile(context) != 0 ||
            av_lease_guest_check(lease, ops->now_ms(context)) != 0) return guest_fail(lease);
        av_message_t ready = {.type = MsgInputEpochReady, .lease_generation = lease->epoch,
                              .buffer_index = message->buffer_index};
        if (ops->publish(context, &ready) != 0) return guest_fail(lease);
        lease->phase = AvLeaseUnfocusedReady; lease->deadline = 0;
        return 0;
    }
    if (lease->phase == AvLeaseAwaitAck) return guest_fail(lease);
    int focus_on = message->type == MsgInputFocusV3 && message->flags;
    if (focus_on) {
        int probe = ops->input.target(context, message->window_id, message->sequence, -1);
        if (probe > 0) return 0; /* Probe before releasing any existing held input. */
        if (probe < 0) return guest_fail(lease);
    } else if (lease->phase != AvLeaseActive || message->window_id != lease->input.window_id ||
               message->sequence != lease->input.incarnation) return 0;
    if (ops->reconcile(context) != 0 || lease->phase == AvLeaseGuestTerminal)
        return guest_fail(lease);
    if (lease->epoch != message->lease_generation) return 0; /* Trigger caused revocation. */
    av_message_t admitted = *message;
    admitted.type -= MsgInputFocusV3 - MsgInputFocus;
    admitted.lease_generation = 0;
    int result = av_input_apply_admitted(&lease->input, &admitted, &ops->input, context);
    if (result < 0) return guest_fail(lease);
    if (result == 0) {
        if (focus_on && lease->input.window_id == message->window_id &&
            lease->input.incarnation == message->sequence) {
            lease->anchor_id = lease->input.window_id;
            lease->anchor_incarnation = lease->input.incarnation;
            lease->ever_anchored = 1;
            lease->phase = AvLeaseActive;
        } else if (!lease->input.window_id) lease->phase = AvLeaseUnfocusedReady;
    }
    if (!lease->input.window_id && lease->phase == AvLeaseActive) lease->phase = AvLeaseUnfocusedReady;
    /* Inserted down/up bits are committed before callbacks can release/revoke. */
    if (ops->reconcile(context) != 0 || av_lease_guest_check(lease, ops->now_ms(context)) != 0)
        return guest_fail(lease);
    return 0;
}
int av_lease_host_receive(av_lease_host_t *lease, const av_message_t *message, uint64_t now_ms) {
    uint8_t bytes[AvControlBytes];
    if (av_lease_host_check(lease, now_ms) != 0) return -1;
    if (!message || (message->type != MsgInputEpochRevoked && message->type != MsgInputEpochReady) ||
        av_control_encode(message, bytes, sizeof(bytes)) != 0) return host_fail(lease);
    if (message->type == MsgInputEpochRevoked) {
        if (lease->epoch == UINT64_MAX || message->lease_generation != lease->epoch + 1 ||
            (lease->phase != AvLeaseAwaitInitialRevocation &&
             lease->phase != AvLeaseHostUnfocusedReady && lease->phase != AvLeaseForwarding) ||
            now_ms > UINT64_MAX - AvLeaseTimeoutMs) return host_fail(lease);
        lease->epoch = message->lease_generation; lease->ack_serial = 0;
        lease->deadline = now_ms + AvLeaseTimeoutMs; lease->phase = AvLeaseBarrier;
        return 1;
    }
    if (message->lease_generation < lease->epoch) return 0;
    if (message->lease_generation != lease->epoch || lease->phase != AvLeaseAwaitReady ||
        message->buffer_index != lease->ack_serial) return host_fail(lease);
    lease->phase = AvLeaseHostUnfocusedReady; lease->deadline = 0;
    return 0;
}
int av_lease_host_ack(av_lease_host_t *lease, uint64_t now_ms, av_message_t *message) {
    if (av_lease_host_check(lease, now_ms) != 0) return -1;
    if (lease->phase != AvLeaseBarrier || lease->serial == UINT32_MAX) return host_fail(lease);
    lease->ack_serial = ++lease->serial;
    *message = (av_message_t){.type = MsgInputEpochAck, .lease_generation = lease->epoch,
                             .buffer_index = lease->ack_serial};
    lease->phase = AvLeaseAwaitReady;
    return 0;
}
int av_lease_host_stamp(av_lease_host_t *lease, av_message_t *message) {
    if (lease->phase != AvLeaseForwarding &&
        !(lease->phase == AvLeaseHostUnfocusedReady && message->type == MsgInputFocusV3 && message->flags))
        return host_fail(lease);
    if (lease->serial == UINT32_MAX) return host_fail(lease);
    av_message_t result = *message;
    result.lease_generation = lease->epoch; result.buffer_index = lease->serial + 1;
    uint8_t bytes[AvControlBytes];
    if (result.type < MsgInputFocusV3 || result.type > MsgInputReleaseV3 ||
        av_control_encode(&result, bytes, sizeof(bytes)) != 0) return host_fail(lease);
    ++lease->serial; *message = result;
    return 0;
}
