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

/* Frozen values. A change to any of these is a protocol change and must fail this test. */
_Static_assert(WaddleWindowStateOk == 0, "status Ok is 0");
_Static_assert(WaddleWindowStateInvalidPlan == 13, "status InvalidPlan is 13");
_Static_assert(WaddleWindowStateActionHostSetMinimized == 1, "first action kind");
_Static_assert(WaddleWindowStateActionGuestPostClose == 13, "last action kind");
_Static_assert(WaddleWindowStateInputDestroyObserved == 6, "last input kind");
_Static_assert(WaddleWindowStatePendingRetire == 5, "last pending kind");

/* Frozen protocol state bits (see window_state.zig). */
#define StateMinimized 0x01u
#define StateMaximized 0x02u
#define StateFullscreen 0x04u
#define StateActivated 0x08u
#define StateMaximizable 0x20u
#define StateMinimizable 0x40u

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
    Check(waddle_window_state_create(1, 1, StateActivated, &handle) == WaddleWindowStateInvalidState);
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
        Check(waddle_window_state_create(100u + round, 1u + round % 3u, StateMaximizable, &handle) == WaddleWindowStateOk);
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

    Check(waddle_window_state_create(7, 2, StateMinimized | StateMaximizable, &handle) == WaddleWindowStateOk);

    /* Reconciliation of a minimized guest: one-shot minimize, then unmaximize, then unfullscreen. */
    input = observation(StateMinimized | StateMaximizable);
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
    Check(snapshot.observed_guest_state == StateMinimized);
    Check(snapshot.guest_capabilities == StateMaximizable);
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
    Check(snapshot.applied_host_state == StateMaximized);

    /* Activation of an iconic guest that is also being maximized: restore, maximize, activate, geometry. */
    input = configure_input(StateActivated | StateMaximized, -20, 10, 1024, 768);
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
    Check(snapshot.applied_guest_state == StateMaximized);

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
    input = configure_input(StateActivated, 0, 0, 640, 480);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &geometry_plan) == WaddleWindowStateOk);
    Check(geometry_plan.action_count == 2);
    Check(waddle_window_state_complete(handle, &geometry_plan, 0, 1) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &after) == WaddleWindowStateOk);
    Check(after.synced == 0);
    Check(after.revision == before.revision + 1);
    /* A failed configure leaves the applied guest state unchanged. */
    Check(after.applied_guest_state == before.applied_guest_state);

    /* While unsynchronized, every host-side request is refused and the plan is zero. */
    input = configure_input(StateActivated, 0, 0, 0, 0);
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

    input = configure_input(StateActivated, 0, 0, 640, 480);
    Check(waddle_window_state_plan(handle, 9, 1, &input, &plan) == WaddleWindowStateOk);
    Check(waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk);

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    return 0;
}

static int test_stale_plans_and_identity(void) {
    waddle_window_state_handle_t *handle = NULL;
    waddle_window_state_input_t close_input = bare_input(WaddleWindowStateInputCloseRequest);
    waddle_window_state_input_t configure = configure_input(StateActivated, 0, 0, 0, 0);
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

    Check(create_synced(13, 1, StateMinimizable, &handle) == 0);
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

    Check(create_synced(12, 3, StateMinimizable, &handle) == 0);
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
    Check(create_synced(17, 2, StateMaximizable, &synced) == 0);
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
    Check(waddle_window_state_create(18, 1, StateMinimizable, &unsynced) == WaddleWindowStateOk);
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
    waddle_window_state_input_t input = configure_input(StateMaximized, 1, 2, 300, 200);
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
    waddle_window_state_snapshot_t first;
    waddle_window_state_snapshot_t second;

    Check(create_synced(16, 4, StateMaximizable, &handle) == 0);
    Check(waddle_window_state_snapshot(handle, &first) == WaddleWindowStateOk);
    Check(waddle_window_state_snapshot(handle, &second) == WaddleWindowStateOk);
    Check(same_snapshot(&first, &second));
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
