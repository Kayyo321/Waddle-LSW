#ifndef WaddleAvLeaseH
#define WaddleAvLeaseH
#include "av_input.h"
/** @brief Fixed cooperative handshake duration in monotonic milliseconds. */
#define AvLeaseTimeoutMs 2000u
/** @brief Guest authority phases, zero initialization is unarmed. */
typedef enum av_lease_guest_phase_t {
    AvLeaseInitial, /**< No successfully exported window yet. */
    AvLeaseAwaitAck, /**< Revoked sent; only matching Ack may advance. */
    AvLeaseUnfocusedReady, /**< Ready sent; a fresh explicit focus is required. */
    AvLeaseActive, /**< Native focus validated synchronously. */
    AvLeaseGuestTerminal /**< No further normal effects or publication. */
} av_lease_guest_phase_t;
/** @brief Host requested-routing phases; Forwarding is not guest acknowledgment. */
typedef enum av_lease_host_phase_t {
    AvLeaseAwaitInitialRevocation, /**< CreateV3 may precede first Revoked. */
    AvLeaseBarrier, /**< Own an ordered Wayland sync callback. */
    AvLeaseAwaitReady, /**< Ack queued; original deadline retained. */
    AvLeaseHostUnfocusedReady, /**< No cached enter is replayed. */
    AvLeaseForwarding, /**< Fresh focus request successfully queued. */
    AvLeaseHostTerminal /**< No further publication. */
} av_lease_host_phase_t;
/** @brief Caller-owned guest lease; zero per connection, owner-thread only. */
typedef struct av_lease_guest_t {
    av_input_state_t input; /**< Sole serial high-water and exact inserted holds. */
    uint64_t epoch, deadline; /**< Independent epoch and nonzero deadline, or zero. */
    uint64_t anchor_id, anchor_incarnation; /**< Native anchor retained without focus. */
    av_lease_guest_phase_t phase; /**< Actual guest authorization phase. */
    int ever_anchored; /**< Sticky until genuinely fresh session initialization. */
} av_lease_guest_t;
/** @brief Caller-owned host lease; zero per connection, owner-thread only. */
typedef struct av_lease_host_t {
    uint64_t epoch, deadline; /**< Current guest epoch and unchanged handshake deadline. */
    uint32_t serial, ack_serial; /**< One host serial authority and exact Ack echo. */
    av_lease_host_phase_t phase; /**< Requested forwarding only, never native authority. */
} av_lease_host_t;
/** @brief Synchronous borrowed guest adapters; no retained message pointers. */
typedef struct av_lease_ops_t {
    av_input_ops_t input; /**< Native target/insertion adapter; no recursive input mutation. */
    int (*publish)(void *context, const av_message_t *message); /**< Required control enqueue: exactly zero succeeds. */
    int (*reconcile)(void *context); /**< Resolve observations/current native evidence, may revoke; negative terminates. */
    uint64_t (*now_ms)(void *context); /**< Checked monotonic time; zero means clock failure. */
} av_lease_ops_t;
/** @brief Start epoch one after first CreateV3 enqueue and metadata commit.
 * @param[in,out] lease Nonnull zero-initialized owned state.
 * @param[in] ops Nonnull complete synchronous table, borrowed for call.
 * @param[in,out] context Optional borrowed adapter context.
 * @return 0 Revoked queued, -1 terminal clock/state/queue failure. Owner thread; no allocation.
 */
int av_lease_guest_begin(av_lease_guest_t *lease, const av_lease_ops_t *ops, void *context);
/** @brief Reconcile one observed eligible exported foreground pair in receipt order.
 * @param[in,out] lease Nonnull owned state; unarmed observations grant no authority.
 * @param[in] window_id Nonzero currently eligible exported candidate.
 * @param[in] incarnation Nonzero immutable candidate token.
 * @param[in] ops Nonnull complete borrowed adapters.
 * @param[in,out] context Optional borrowed callback context.
 * @return 0 unchanged/revoked, -1 terminal overlap/exhaustion/release/queue error.
 * @note Owner thread only; releases before Revoked, never recursively from emit.
 */
int av_lease_guest_transition(av_lease_guest_t *lease, uint64_t window_id,
    uint64_t incarnation, const av_lease_ops_t *ops, void *context);
/** @brief Synchronously admit one V3 host frame and complete its native effects.
 * @param[in,out] lease Nonnull owned V3 state.
 * @param[in] message Nonnull borrowed Ack or V3 input.
 * @param[in] ops Nonnull complete borrowed adapters.
 * @param[in,out] context Optional borrowed context.
 * @return 0 accepted/ordered no-op, -1 terminal. Stale epochs consume serial once
 * without native reconciliation; caller must separately reconcile loop boundaries.
 * @note Owner thread; no allocation, no deferred focus or result handshake.
 */
int av_lease_guest_apply(av_lease_guest_t *lease, const av_message_t *message,
    const av_lease_ops_t *ops, void *context);
/** @brief Check guest deadline at owner-loop boundaries without extending it.
 * @param[in,out] lease Nonnull owned state. @param[in] now_ms Nonzero monotonic time.
 * @return 0 live, -1 terminal/clock/expiry; owner thread, no allocation/effects.
 */
int av_lease_guest_check(av_lease_guest_t *lease, uint64_t now_ms);
/** @brief Admit guest Revoked/Ready before any window or video-slot lookup.
 * @param[in,out] lease Nonnull owned V3 host state.
 * @param[in] message Nonnull borrowed canonical lease control.
 * @param[in] now_ms Nonzero monotonic time.
 * @return 1 create barrier, 0 Ready/old Ready no-op, -1 terminal malformed/phase/deadline.
 * @note Owner thread; no allocation. Caller disables routes before creating barrier.
 */
int av_lease_host_receive(av_lease_host_t *lease, const av_message_t *message, uint64_t now_ms);
/** @brief Produce Ack for the exact live owned barrier, retaining original deadline.
 * @param[in,out] lease Nonnull owned host state.
 * @param[in] now_ms Nonzero monotonic time. @param[out] message Nonnull caller output.
 * @return 0 Ack prepared, -1 terminal. Caller must enqueue exactly once; failure terminal.
 * @note Owner thread, allocation-free; callback identity is checked by Wayland adapter.
 */
int av_lease_host_ack(av_lease_host_t *lease, uint64_t now_ms, av_message_t *message);
/** @brief Stamp a V3 input with next serial and current epoch, without enqueueing.
 * @param[in,out] lease Nonnull owned state. @param[in,out] message Nonnull caller-owned
 * canonical event except serial/epoch, changed only on success.
 * @return 0 stamped, -1 terminal malformed/phase/exhaustion. Owner thread, no allocation.
 * @note Focus-on is permitted after Ready; adapter commits Forwarding only on enqueue success.
 */
int av_lease_host_stamp(av_lease_host_t *lease, av_message_t *message);
/** @brief Check host handshake deadline without extending it.
 * @param[in,out] lease Nonnull owned state. @param[in] now_ms Nonzero monotonic time.
 * @return 0 live, -1 terminal/clock/expiry. Owner thread, no allocation/effects.
 */
int av_lease_host_check(av_lease_host_t *lease, uint64_t now_ms);
/** @brief Return bounded wait duration to the nearest handshake deadline.
 * @param[in] deadline Zero for no handshake, else absolute monotonic milliseconds.
 * @param[in] now_ms Nonzero checked monotonic time. @param[in] maximum Nonnegative cap.
 * @return 0..maximum milliseconds, -1 invalid clock/cap or expired. Pure, thread-safe.
 */
int av_lease_timeout(uint64_t deadline, uint64_t now_ms, int maximum);
#endif
