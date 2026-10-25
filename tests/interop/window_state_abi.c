/**
 * @file window_state_abi.c
 * @brief C ABI consumer tests for the window-state reducer (src/interop/window_state.h).
 *
 * Links against the Zig ABI object only and sees nothing but the public header. Every test
 * returns 0 on success and 1 at the first failed Check. main() runs every test and exits
 * nonzero when any test fails. Every handle created here is destroyed, so the sanitizer build
 * reports no leak. Pointer arguments are always naturally aligned; the NULL and invalid-argument
 * tests cover the cases the ABI rejects.
 */
#include "window_state.h"
#include "waddle/interop_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Layout contract. These must agree with the comptime checks in window_state_abi.zig. */
_Static_assert(sizeof(waddle_window_state_input_t) == 28, "input is 28 bytes");
_Static_assert(_Alignof(waddle_window_state_input_t) == 4, "input alignment is 4");
_Static_assert(sizeof(waddle_window_state_action_t) == 24, "action is 24 bytes");
_Static_assert(_Alignof(waddle_window_state_action_t) == 4, "action alignment is 4");
_Static_assert(sizeof(waddle_window_state_plan_t) == 248, "plan is 248 bytes");
_Static_assert(_Alignof(waddle_window_state_plan_t) == 8, "plan alignment is 8");
_Static_assert(offsetof(waddle_window_state_plan_t, actions) == 52, "plan actions start at offset 52");
_Static_assert(sizeof(waddle_window_state_snapshot_t) == 56, "snapshot is 56 bytes");
_Static_assert(_Alignof(waddle_window_state_snapshot_t) == 8, "snapshot alignment is 8");
_Static_assert(WaddleWindowStateMaxActions == 8, "plan capacity is 8 actions");

/* Field offsets. Values are the byte offsets documented in window_state_abi_contract.md section 2. */
_Static_assert(offsetof(waddle_window_state_input_t, kind) == 0, "input.kind offset");
_Static_assert(offsetof(waddle_window_state_input_t, state) == 4, "input.state offset");
_Static_assert(offsetof(waddle_window_state_input_t, x) == 8, "input.x offset");
_Static_assert(offsetof(waddle_window_state_input_t, y) == 12, "input.y offset");
_Static_assert(offsetof(waddle_window_state_input_t, width) == 16, "input.width offset");
_Static_assert(offsetof(waddle_window_state_input_t, height) == 20, "input.height offset");
_Static_assert(offsetof(waddle_window_state_input_t, maximize) == 24, "input.maximize offset");
_Static_assert(offsetof(waddle_window_state_action_t, kind) == 0, "action.kind offset");
_Static_assert(offsetof(waddle_window_state_action_t, x) == 4, "action.x offset");
_Static_assert(offsetof(waddle_window_state_action_t, y) == 8, "action.y offset");
_Static_assert(offsetof(waddle_window_state_action_t, width) == 12, "action.width offset");
_Static_assert(offsetof(waddle_window_state_action_t, height) == 16, "action.height offset");
_Static_assert(offsetof(waddle_window_state_action_t, maximized) == 20, "action.maximized offset");
_Static_assert(offsetof(waddle_window_state_plan_t, window_id) == 0, "plan.window_id offset");
_Static_assert(offsetof(waddle_window_state_plan_t, base_revision) == 8, "plan.base_revision offset");
_Static_assert(offsetof(waddle_window_state_plan_t, action_count) == 16, "plan.action_count offset");
_Static_assert(offsetof(waddle_window_state_plan_t, seal) == 24, "plan.seal offset");
_Static_assert(offsetof(waddle_window_state_plan_t, generation) == 32, "plan.generation offset");
_Static_assert(offsetof(waddle_window_state_plan_t, pending_kind) == 36, "plan.pending_kind offset");
_Static_assert(offsetof(waddle_window_state_plan_t, pending_state) == 40, "plan.pending_state offset");
_Static_assert(offsetof(waddle_window_state_plan_t, pending_capabilities) == 44, "plan.pending_capabilities offset");
_Static_assert(offsetof(waddle_window_state_plan_t, pending_host_state) == 48, "plan.pending_host_state offset");
_Static_assert(offsetof(waddle_window_state_plan_t, actions) == 52, "plan.actions offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, window_id) == 0, "snapshot.window_id offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, revision) == 8, "snapshot.revision offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, generation) == 16, "snapshot.generation offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, observed_guest_state) == 20, "snapshot.observed_guest_state offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, applied_guest_state) == 24, "snapshot.applied_guest_state offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, applied_host_state) == 28, "snapshot.applied_host_state offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, guest_capabilities) == 32, "snapshot.guest_capabilities offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, host_request) == 36, "snapshot.host_request offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, synced) == 40, "snapshot.synced offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, close_posted) == 44, "snapshot.close_posted offset");
_Static_assert(offsetof(waddle_window_state_snapshot_t, retired) == 48, "snapshot.retired offset");

/* Enumerated values. Every enumerator of the header, with the value documented in the contract section 4. */
_Static_assert(WaddleWindowStateOk == 0, "status Ok");
_Static_assert(WaddleWindowStateInvalidArgument == 1, "status InvalidArgument");
_Static_assert(WaddleWindowStateOutOfMemory == 2, "status OutOfMemory");
_Static_assert(WaddleWindowStateInvalidIdentity == 3, "status InvalidIdentity");
_Static_assert(WaddleWindowStateInvalidState == 4, "status InvalidState");
_Static_assert(WaddleWindowStateInvalidGeometry == 5, "status InvalidGeometry");
_Static_assert(WaddleWindowStateStaleWindow == 6, "status StaleWindow");
_Static_assert(WaddleWindowStateStalePlan == 7, "status StalePlan");
_Static_assert(WaddleWindowStateRetired == 8, "status Retired");
_Static_assert(WaddleWindowStateUnsynchronized == 9, "status Unsynchronized");
_Static_assert(WaddleWindowStateUnsupportedStateRequest == 10, "status UnsupportedStateRequest");
_Static_assert(WaddleWindowStateRevisionExhausted == 11, "status RevisionExhausted");
_Static_assert(WaddleWindowStateInvalidCompletion == 12, "status InvalidCompletion");
_Static_assert(WaddleWindowStateInvalidPlan == 13, "status InvalidPlan");
_Static_assert(WaddleWindowStateInputGuestObservation == 1, "input GuestObservation");
_Static_assert(WaddleWindowStateInputHostConfigure == 2, "input HostConfigure");
_Static_assert(WaddleWindowStateInputHostStateRequest == 3, "input HostStateRequest");
_Static_assert(WaddleWindowStateInputCloseRequest == 4, "input CloseRequest");
_Static_assert(WaddleWindowStateInputHideObserved == 5, "input HideObserved");
_Static_assert(WaddleWindowStateInputDestroyObserved == 6, "input DestroyObserved");
_Static_assert(WaddleWindowStateActionHostSetMinimized == 1, "action HostSetMinimized");
_Static_assert(WaddleWindowStateActionHostSetMaximized == 2, "action HostSetMaximized");
_Static_assert(WaddleWindowStateActionHostUnsetMaximized == 3, "action HostUnsetMaximized");
_Static_assert(WaddleWindowStateActionHostSetFullscreen == 4, "action HostSetFullscreen");
_Static_assert(WaddleWindowStateActionHostUnsetFullscreen == 5, "action HostUnsetFullscreen");
_Static_assert(WaddleWindowStateActionGuestRestore == 6, "action GuestRestore");
_Static_assert(WaddleWindowStateActionGuestMaximize == 7, "action GuestMaximize");
_Static_assert(WaddleWindowStateActionGuestMinimize == 8, "action GuestMinimize");
_Static_assert(WaddleWindowStateActionGuestEnterFullscreen == 9, "action GuestEnterFullscreen");
_Static_assert(WaddleWindowStateActionGuestLeaveFullscreen == 10, "action GuestLeaveFullscreen");
_Static_assert(WaddleWindowStateActionGuestActivate == 11, "action GuestActivate");
_Static_assert(WaddleWindowStateActionGuestSetGeometry == 12, "action GuestSetGeometry");
_Static_assert(WaddleWindowStateActionGuestPostClose == 13, "action GuestPostClose");
_Static_assert(WaddleWindowStatePendingGuestObservation == 1, "pending GuestObservation");
_Static_assert(WaddleWindowStatePendingHostConfigure == 2, "pending HostConfigure");
_Static_assert(WaddleWindowStatePendingHostStateRequest == 3, "pending HostStateRequest");
_Static_assert(WaddleWindowStatePendingCloseRequest == 4, "pending CloseRequest");
_Static_assert(WaddleWindowStatePendingRetire == 5, "pending Retire");
_Static_assert(WaddleWindowStateHostRequestNone == 0, "host request None");
_Static_assert(WaddleWindowStateHostRequestUnmaximize == 1, "host request Unmaximize");
_Static_assert(WaddleWindowStateHostRequestMaximize == 2, "host request Maximize");

/* Protocol state bits: each equals its InteropState* enumerator in include/waddle/interop_protocol.h. */
_Static_assert(WaddleWindowStateMinimized == InteropStateMinimized, "Minimized bit");
_Static_assert(WaddleWindowStateMaximized == InteropStateMaximized, "Maximized bit");
_Static_assert(WaddleWindowStateFullscreen == InteropStateFullscreen, "Fullscreen bit");
_Static_assert(WaddleWindowStateActivated == InteropStateActivated, "Activated bit");
_Static_assert(WaddleWindowStateResizable == InteropStateResizable, "Resizable bit");
_Static_assert(WaddleWindowStateMaximizable == InteropStateMaximizable, "Maximizable bit");
_Static_assert(WaddleWindowStateMinimizable == InteropStateMinimizable, "Minimizable bit");
_Static_assert(WaddleWindowStateMask == InteropStateMask, "state mask");

/* Frozen values. A change to any of these is a protocol change and must fail this test. */

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "window_state_abi.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static waddle_window_state_input_t observation(uint32_t state) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputGuestObservation;
    input.state = state;
    return input;
}

static waddle_window_state_input_t configure_input(uint32_t state, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputHostConfigure;
    input.state = state;
    input.x = x;
    input.y = y;
    input.width = width;
    input.height = height;
    return input;
}

static waddle_window_state_input_t request_input(uint32_t maximize) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputHostStateRequest;
    input.maximize = maximize;
    return input;
}

static waddle_window_state_input_t bare_input(uint32_t kind) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = kind;
    return input;
}

static int plan_is_zero(const waddle_window_state_plan_t *plan) {
    waddle_window_state_plan_t zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(plan, &zero, sizeof(zero)) == 0;
}

static int same_snapshot(const waddle_window_state_snapshot_t *left, const waddle_window_state_snapshot_t *right) {
    return memcmp(left, right, sizeof(*left)) == 0;
}

/* Creates a handle for an observed guest, then plans and completes its reconciliation. */
static int create_synced(uint64_t window_id, uint32_t generation, uint32_t guest_state, waddle_window_state_handle_t **out) {
    waddle_window_state_input_t input = observation(guest_state);
    waddle_window_state_plan_t plan;
    if (waddle_window_state_create(window_id, generation, guest_state, out) != WaddleWindowStateOk) return 1;
    if (waddle_window_state_plan(*out, window_id, generation, &input, &plan) != WaddleWindowStateOk) return 1;
    if (waddle_window_state_complete(*out, &plan, 1, plan.action_count) != WaddleWindowStateOk) return 1;
    return 0;
}

static int test_invalid_arguments(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t input = observation(0);
    waddle_window_state_plan_t plan;
    waddle_window_state_snapshot_t snapshot;

    Check(waddle_window_state_create(1, 1, 0, NULL) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_create(0, 1, 0, &handle) == WaddleWindowStateInvalidIdentity);
    Check(handle == NULL);
    Check(waddle_window_state_create(1, 0, 0, &handle) == WaddleWindowStateInvalidIdentity);
    Check(handle == NULL);
    Check(waddle_window_state_create(1, 1, WaddleWindowStateActivated, &handle) == WaddleWindowStateInvalidState);
    Check(handle == NULL);
    Check(waddle_window_state_create(1, 1, 0x80u, &handle) == WaddleWindowStateInvalidState);
    Check(handle == NULL);

    Check(waddle_window_state_create(1, 1, 0, &handle) == WaddleWindowStateOk);
    Check(handle != NULL);

    memset(&plan, 0xa5, sizeof(plan));
    Check(waddle_window_state_plan(NULL, 1, 1, &input, &plan) == WaddleWindowStateInvalidArgument);
    Check(plan_is_zero(&plan));
    memset(&plan, 0xa5, sizeof(plan));
    Check(waddle_window_state_plan(handle, 1, 1, NULL, &plan) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_plan(handle, 1, 1, &input, NULL) == WaddleWindowStateInvalidArgument);

    input.kind = 0;
    Check(waddle_window_state_plan(handle, 1, 1, &input, &plan) == WaddleWindowStateInvalidArgument);
    Check(plan_is_zero(&plan));
    input.kind = 7;
    Check(waddle_window_state_plan(handle, 1, 1, &input, &plan) == WaddleWindowStateInvalidArgument);
    input = request_input(2);
    Check(waddle_window_state_plan(handle, 1, 1, &input, &plan) == WaddleWindowStateInvalidArgument);
    Check(plan_is_zero(&plan));

    Check(waddle_window_state_complete(NULL, &plan, 1, 0) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_complete(handle, NULL, 1, 0) == WaddleWindowStateInvalidArgument);
    memset(&plan, 0, sizeof(plan));
    Check(waddle_window_state_complete(handle, &plan, 2, 0) == WaddleWindowStateInvalidArgument);

    Check(waddle_window_state_snapshot(NULL, &snapshot) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_snapshot(handle, NULL) == WaddleWindowStateInvalidArgument);

    Check(waddle_window_state_destroy(NULL) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    Check(handle == NULL);
    return 0;
}

static int test_lifecycle_and_slot_clearing(void) {
    waddle_window_state_handle_t *live[64];
    uint32_t round;

    for (round = 0; round < 512; round++) {
        waddle_window_state_handle_t *handle = NULL;
        Check(waddle_window_state_create(100u + round, 1u + round % 3u, WaddleWindowStateMaximizable, &handle) == WaddleWindowStateOk);
        Check(handle != NULL);
        Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
        Check(handle == NULL);
        /* A cleared slot is a successful no-op, so a second destroy cannot double free. */
        Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    }

    for (round = 0; round < 64; round++) {
        live[round] = NULL;
        Check(waddle_window_state_create(500u + round, 2, 0, &live[round]) == WaddleWindowStateOk);
    }
    for (round = 64; round > 0; round--) {
        Check(waddle_window_state_destroy(&live[round - 1]) == WaddleWindowStateOk);
        Check(live[round - 1] == NULL);
    }
    return 0;
}

static int test_ticket_flow_and_action_order(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t input;
    waddle_window_state_plan_t plan;
    waddle_window_state_snapshot_t snapshot;

    Check(waddle_window_state_create(7, 2, WaddleWindowStateMinimized | WaddleWindowStateMaximizable, &handle) == WaddleWindowStateOk);

    /* Reconciliation of a minimized guest: one-shot minimize, then unmaximize, then unfullscreen. */
    input = observation(WaddleWindowStateMinimized | WaddleWindowStateMaximizable);
    Check(waddle_window_state_plan(handle, 7, 2, &input, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 3);
    Check(plan.actions[0].kind == WaddleWindowStateActionHostSetMinimized);
    Check(plan.actions[1].kind == WaddleWindowStateActionHostUnsetMaximized);
    Check(plan.actions[2].kind == WaddleWindowStateActionHostUnsetFullscreen);
    Check(waddle_window_state_complete(handle, &plan, 1, 3) == WaddleWindowStateOk);

    Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.window_id == 7);
    Check(snapshot.generation == 2);
    Check(snapshot.revision == 2);
    Check(snapshot.synced == 1);
    Check(snapshot.observed_guest_state == WaddleWindowStateMinimized);
    Check(snapshot.guest_capabilities == WaddleWindowStateMaximizable);
    Check(snapshot.applied_host_state == 0);
    Check(snapshot.host_request == WaddleWindowStateHostRequestNone);
    Check(snapshot.retired == 0);

    /* Maximize policy: needs the Maximizable capability and completes into the host state. */
    input = request_input(1);
    Check(waddle_window_state_plan(handle, 7, 2, &input, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 1);
    Check(plan.actions[0].kind == WaddleWindowStateActionHostSetMaximized);
    Check(waddle_window_state_complete(handle, &plan, 1, 1) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.host_request == WaddleWindowStateHostRequestMaximize);
    Check(snapshot.applied_host_state == WaddleWindowStateMaximized);

    /* Activation of an iconic guest that is also being maximized: restore, maximize, activate, geometry. */
    input = configure_input(WaddleWindowStateActivated | WaddleWindowStateMaximized, -20, 10, 1024, 768);
    Check(waddle_window_state_plan(handle, 7, 2, &input, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 4);
    Check(plan.actions[0].kind == WaddleWindowStateActionGuestRestore);
    Check(plan.actions[1].kind == WaddleWindowStateActionGuestMaximize);
    Check(plan.actions[2].kind == WaddleWindowStateActionGuestActivate);
    Check(plan.actions[3].kind == WaddleWindowStateActionGuestSetGeometry);
    Check(plan.actions[3].x == -20);
    Check(plan.actions[3].y == 10);
    Check(plan.actions[3].width == 1024);
    Check(plan.actions[3].height == 768);
    Check(plan.actions[3].maximized == 1);
    /* Actions beyond the count are not part of the plan and are zero. */
    Check(plan.actions[4].kind == 0);
    Check(waddle_window_state_complete(handle, &plan, 1, 4) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.applied_guest_state == WaddleWindowStateMaximized);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_partial_failure_and_resynchronization(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t input;
    waddle_window_state_plan_t plan;
    waddle_window_state_plan_t geometry_plan;
    waddle_window_state_snapshot_t before;
    waddle_window_state_snapshot_t after;

    Check(create_synced(9, 1, 0, &handle) == 0);
    Check(waddle_window_state_snapshot(handle, &before) == WaddleWindowStateOk);

    /* Activation with geometry: two actions. Report only the first as completed. */
    input = configure_input(WaddleWindowStateActivated, 0, 0, 640, 480);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &geometry_plan) == WaddleWindowStateOk);
    Check(geometry_plan.action_count == 2);
    Check(waddle_window_state_complete(handle, &geometry_plan, 0, 1) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(after.synced == 0);
    Check(after.revision == before.revision + 1);
    /* A failed configure leaves the applied guest state unchanged. */
    Check(after.applied_guest_state == before.applied_guest_state);

    /* While unsynchronized, every host-side request is refused and the plan is zero. */
    input = configure_input(WaddleWindowStateActivated, 0, 0, 0, 0);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &plan) == WaddleWindowStateUnsynchronized);
    Check(plan_is_zero(&plan));
    input = request_input(0);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &plan) == WaddleWindowStateUnsynchronized);

    /* A fresh observation resynchronizes; the old geometry plan is now stale. */
    input = observation(0);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(after.synced == 1);
    Check(waddle_window_state_complete(handle, &geometry_plan, 1, 2) == WaddleWindowStateStalePlan);

    input = configure_input(WaddleWindowStateActivated, 0, 0, 640, 480);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_stale_plans_and_identity(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t close_input = bare_input(WaddleWindowStateInputCloseRequest);
    waddle_window_state_input_t configure = configure_input(WaddleWindowStateActivated, 0, 0, 0, 0);
    waddle_window_state_input_t observe_zero = observation(0);
    waddle_window_state_plan_t first;
    waddle_window_state_plan_t second;
    waddle_window_state_plan_t observed;
    waddle_window_state_plan_t activated;

    Check(create_synced(11, 1, 0, &handle) == 0);

    /* Identity must match the bound window and generation exactly. */
    Check(waddle_window_state_plan(handle, 12, 1, &close_input, &first) == WaddleWindowStateStaleWindow);
    Check(waddle_window_state_plan(handle, 11, 2, &close_input, &first) == WaddleWindowStateStaleWindow);
    Check(plan_is_zero(&first));

    Check(waddle_window_state_plan(handle, 11, 1, &close_input, &first) == WaddleWindowStateOk);
    Check(waddle_window_state_plan(handle, 11, 1, &close_input, &second) == WaddleWindowStateOk);
    Check(first.action_count == 1);
    Check(first.actions[0].kind == WaddleWindowStateActionGuestPostClose);

    /* Two plans from the same revision: the first completion wins, the second is stale. */
    Check(waddle_window_state_complete(handle, &first, 1, 1) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &second, 1, 1) == WaddleWindowStateStalePlan);
    Check(waddle_window_state_complete(handle, &first, 1, 1) == WaddleWindowStateStalePlan);

    /* A zero-action observation and a configure planned before it: the observation completes, the configure is stale. */
    Check(waddle_window_state_plan(handle, 11, 1, &observe_zero, &observed) == WaddleWindowStateOk);
    Check(observed.action_count == 0);
    Check(waddle_window_state_plan(handle, 11, 1, &configure, &activated) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &observed, 1, 0) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &activated, 1, 1) == WaddleWindowStateStalePlan);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_maximize_capability_gate(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t input = request_input(1);
    waddle_window_state_plan_t plan;

    Check(create_synced(13, 1, WaddleWindowStateMinimizable, &handle) == 0);
    Check(waddle_window_state_plan(handle, 13, 1, &input, &plan) == WaddleWindowStateUnsupportedStateRequest);
    Check(plan_is_zero(&plan));
    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_retirement(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t hide = bare_input(WaddleWindowStateInputHideObserved);
    waddle_window_state_input_t destroyed = bare_input(WaddleWindowStateInputDestroyObserved);
    waddle_window_state_input_t close_input = bare_input(WaddleWindowStateInputCloseRequest);
    waddle_window_state_plan_t retire;
    waddle_window_state_plan_t plan;
    waddle_window_state_snapshot_t snapshot;

    Check(create_synced(12, 3, WaddleWindowStateMinimizable, &handle) == 0);
    Check(waddle_window_state_plan(handle, 12, 3, &hide, &retire) == WaddleWindowStateOk);
    Check(retire.action_count == 0);
    /* A retirement completes only as a zero-action success. */
    Check(waddle_window_state_complete(handle, &retire, 0, 0) == WaddleWindowStateInvalidCompletion);
    Check(waddle_window_state_complete(handle, &retire, 1, 1) == WaddleWindowStateInvalidCompletion);
    Check(waddle_window_state_complete(handle, &retire, 1, 0) == WaddleWindowStateOk);

    Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.retired == 1);

    Check(waddle_window_state_plan(handle, 12, 3, &close_input, &plan) == WaddleWindowStateRetired);
    Check(plan_is_zero(&plan));
    Check(waddle_window_state_plan(handle, 12, 3, &destroyed, &plan) == WaddleWindowStateRetired);
    Check(waddle_window_state_complete(handle, &retire, 1, 0) == WaddleWindowStateRetired);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_destroy_observation_retires_without_native_action(void) {
    waddle_window_state_handle_t *synced = NULL;
    waddle_window_state_handle_t *unsynced = NULL;
    waddle_window_state_input_t destroyed = bare_input(WaddleWindowStateInputDestroyObserved);
    waddle_window_state_input_t close_input = bare_input(WaddleWindowStateInputCloseRequest);
    waddle_window_state_plan_t plan;
    waddle_window_state_snapshot_t snapshot;

    /* Live, synchronized window: the destroy plan carries no action, so no native call is made. */
    Check(create_synced(17, 2, WaddleWindowStateMaximizable, &synced) == 0);
    Check(waddle_window_state_plan(synced, 17, 2, &destroyed, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 0);
    Check(plan.pending_kind == WaddleWindowStatePendingRetire);
    Check(waddle_window_state_complete(synced, &plan, 1, 0) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(synced, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.retired == 1);
    Check(snapshot.close_posted == 0);
    Check(snapshot.applied_guest_state == 0);
    Check(snapshot.applied_host_state == 0);
    Check(waddle_window_state_plan(synced, 17, 2, &close_input, &plan) == WaddleWindowStateRetired);
    Check(waddle_window_state_destroy(&synced) == WaddleWindowStateOk);

    /* Unsynchronized window: destroy is accepted without a fresh observation and still carries no action. */
    Check(waddle_window_state_create(18, 1, WaddleWindowStateMinimizable, &unsynced) == WaddleWindowStateOk);
    Check(waddle_window_state_plan(unsynced, 18, 1, &destroyed, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 0);
    Check(waddle_window_state_complete(unsynced, &plan, 1, 0) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(unsynced, &snapshot) == WaddleWindowStateOk);
    Check(snapshot.retired == 1);
    Check(snapshot.close_posted == 0);
    Check(waddle_window_state_destroy(&unsynced) == WaddleWindowStateOk);
    return 0;
}

static int test_forged_and_foreign_plans(void) {
    waddle_window_state_handle_t *owner = NULL;
    waddle_window_state_handle_t *other = NULL;
    waddle_window_state_input_t input = configure_input(WaddleWindowStateMaximized, 1, 2, 300, 200);
    waddle_window_state_plan_t plan;
    waddle_window_state_plan_t forged;
    waddle_window_state_plan_t zero;
    waddle_window_state_snapshot_t before;
    waddle_window_state_snapshot_t foreign_before;
    waddle_window_state_snapshot_t after;

    Check(create_synced(13, 1, 0, &owner) == 0);
    Check(create_synced(13, 1, 0, &other) == 0);
    Check(waddle_window_state_plan(owner, 13, 1, &input, &plan) == WaddleWindowStateOk);
    Check(plan.action_count == 2);
    Check(plan.actions[0].kind == WaddleWindowStateActionGuestMaximize);
    Check(plan.actions[1].kind == WaddleWindowStateActionGuestSetGeometry);
    Check(plan.actions[1].maximized == 1);
    Check(waddle_window_state_snapshot(owner, &before) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(other, &foreign_before) == WaddleWindowStateOk);

    memcpy(&forged, &plan, sizeof(forged));
    forged.window_id = 14;
    Check(waddle_window_state_complete(owner, &forged, 1, 2) == WaddleWindowStateInvalidPlan);

    memcpy(&forged, &plan, sizeof(forged));
    forged.actions[1].width = 301;
    Check(waddle_window_state_complete(owner, &forged, 1, 2) == WaddleWindowStateInvalidPlan);

    memcpy(&forged, &plan, sizeof(forged));
    forged.pending_state = 0;
    Check(waddle_window_state_complete(owner, &forged, 1, 2) == WaddleWindowStateInvalidPlan);

    /* The seal is per handle, so an identical-looking plan from another handle is refused. */
    Check(waddle_window_state_complete(other, &plan, 1, 2) == WaddleWindowStateInvalidPlan);
    Check(waddle_window_state_snapshot(other, &after) == WaddleWindowStateOk);
    Check(same_snapshot(&foreign_before, &after));

    memset(&zero, 0, sizeof(zero));
    Check(waddle_window_state_complete(owner, &zero, 1, 0) == WaddleWindowStateInvalidPlan);

    Check(waddle_window_state_snapshot(owner, &after) == WaddleWindowStateOk);
    Check(same_snapshot(&before, &after));

    /* The untouched plan still completes exactly once. */
    Check(waddle_window_state_complete(owner, &plan, 1, 2) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(owner, &plan, 1, 2) == WaddleWindowStateStalePlan);

    Check(waddle_window_state_destroy(&owner) == WaddleWindowStateOk);
    Check(waddle_window_state_destroy(&other) == WaddleWindowStateOk);
    return 0;
}

static int test_snapshot_is_read_only(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_snapshot_t before;
    waddle_window_state_snapshot_t after;
    waddle_window_state_plan_t plan;
    waddle_window_state_plan_t rejected;
    waddle_window_state_input_t input;
    waddle_window_state_input_t close_input = bare_input(WaddleWindowStateInputCloseRequest);
    waddle_window_state_input_t observe = observation(WaddleWindowStateMinimizable);
    waddle_window_state_input_t maximize;

    Check(create_synced(16, 4, WaddleWindowStateMaximizable, &handle) == 0);
    Check(waddle_window_state_snapshot(handle, &before) == WaddleWindowStateOk);

    /* Planning, in every form, changes no snapshot field: the revision and the state stay exact. */
    input = configure_input(WaddleWindowStateActivated, 0, 0, 640, 480);
    Check(waddle_window_state_plan(handle, 16, 4, &input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(memcmp(&before, &after, sizeof(before)) == 0);

    Check(waddle_window_state_plan(handle, 16, 4, &close_input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_plan(handle, 16, 4, &observe, &plan) == WaddleWindowStateOk);
    maximize = request_input(1);
    Check(waddle_window_state_plan(handle, 16, 4, &maximize, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(memcmp(&before, &after, sizeof(before)) == 0);

    /* Refused plans and refused completions leave the snapshot exact as well. */
    input = bare_input(9);
    Check(waddle_window_state_plan(handle, 16, 4, &input, &rejected) == WaddleWindowStateInvalidArgument);
    Check(waddle_window_state_plan(handle, 17, 4, &close_input, &rejected) == WaddleWindowStateStaleWindow);
    Check(waddle_window_state_complete(handle, &plan, 1, 99) == WaddleWindowStateInvalidCompletion);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(memcmp(&before, &after, sizeof(before)) == 0);

    /* A successful completion is the only operation that moves the snapshot. */
    Check(waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(after.revision == before.revision + 1);
    Check(after.window_id == before.window_id);
    Check(after.generation == before.generation);
    Check(after.synced == 1);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int run(const char *name, int (*test)(void)) {
    int result = test();
    if (result != 0) {
        fprintf(stderr, "FAILED: %s\n", name);
    } else {
        printf("ok: %s\n", name);
    }
    return result;
}

int main(void) {
    int failures = 0;
    failures += run("invalid arguments", test_invalid_arguments);
    failures += run("lifecycle and slot clearing", test_lifecycle_and_slot_clearing);
    failures += run("ticket flow and action order", test_ticket_flow_and_action_order);
    failures += run("partial failure and resynchronization", test_partial_failure_and_resynchronization);
    failures += run("stale plans and identity", test_stale_plans_and_identity);
    failures += run("maximize capability gate", test_maximize_capability_gate);
    failures += run("retirement", test_retirement);
    failures += run("destroy observation retires without native action", test_destroy_observation_retires_without_native_action);
    failures += run("forged and foreign plans", test_forged_and_foreign_plans);
    failures += run("snapshot is read only", test_snapshot_is_read_only);
    if (failures != 0) {
        fprintf(stderr, "window_state_abi: %d test(s) failed\n", failures);
        return 1;
    }
    printf("window_state_abi: all tests passed\n");
    return 0;
}
