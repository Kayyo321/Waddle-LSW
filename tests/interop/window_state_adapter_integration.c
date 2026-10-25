/**
 * @file window_state_adapter_integration.c
 * @brief Integration of the reducer (window_state.h C ABI) with the Win32 adapter over the double.
 *
 * Links the Zig ABI object build/window_state_abi/window_state_abi.o and the adapter. Each round
 * drives the reducer through plan, execute, complete, observe and resynchronization, with native
 * faults injected into the double. The invariant checked after every step, with faults disabled, is
 * that once the machine is synchronized its applied guest window bits equal the adapter's observation.
 * Every host-side plan is completed as successful; the adapter executes only guest actions.
 */
#include "window_state.h"
#include "window_state_adapter_double.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "window_state_adapter_integration.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define HwndMain ((uintptr_t)0x1000)
#define IdMain ((uint64_t)1)
#define GenMain ((uint32_t)1)
#define OwnerThread ((uint32_t)100)

/** @brief The reducer's guest window bits, as the adapter observes them. */
#define WindowBits (WaddleWindowStateMinimized | WaddleWindowStateMaximized | WaddleWindowStateFullscreen)

static uint64_t prng = UINT64_C(0x853c49e6748fea9b);

static uint32_t draw(uint32_t bound) {
    prng ^= prng << 13;
    prng ^= prng >> 7;
    prng ^= prng << 17;
    return (uint32_t)(prng % bound);
}

/** @brief True for the kinds the adapter executes (the guest kinds of window_state.h, 6 to 13). */
static int is_guest_kind(uint32_t kind) {
    return kind >= WaddleWindowStateActionGuestRestore && kind <= WaddleWindowStateActionGuestPostClose;
}

/** @brief Observes the window with faults disabled and returns its guest state bits. */
static window_state_adapter_status_t observe_clean(window_state_adapter_t *adapter, wsa_double_t *d, uint32_t *state) {
    uint32_t saved_rate = d->fault_per_mille;
    window_state_adapter_status_t status;
    d->fault_per_mille = 0;
    status = window_state_adapter_observe(adapter, HwndMain, IdMain, GenMain, state);
    d->fault_per_mille = saved_rate;
    return status;
}

/** @brief Plans a GuestObservation of the current native state and completes its host actions. */
static int resynchronize(waddle_window_state_handle_t *handle, window_state_adapter_t *adapter, wsa_double_t *d) {
    uint32_t state = 0;
    waddle_window_state_input_t input;
    waddle_window_state_plan_t plan;
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputGuestObservation;
    if (observe_clean(adapter, d, &state) != WindowStateAdapterOk) return 1;
    input.state = state;
    if (waddle_window_state_plan(handle, IdMain, GenMain, &input, &plan) != WaddleWindowStateOk) return 1;
    for (uint64_t index = 0; index < plan.action_count; ++index) {
        if (is_guest_kind(plan.actions[index].kind)) return 1;
    }
    return waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk ? 0 : 1;
}

/** @brief Checks the convergence invariant with faults disabled. */
static int check_converged(waddle_window_state_handle_t *handle, window_state_adapter_t *adapter, wsa_double_t *d) {
    waddle_window_state_snapshot_t snapshot;
    uint32_t state = 0;
    Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
    if (snapshot.synced == 0 || snapshot.retired != 0) return 0;
    Check(observe_clean(adapter, d, &state) == WindowStateAdapterOk);
    Check((snapshot.applied_guest_state & WindowBits) == (state & WindowBits));
    return 0;
}

/**
 * @brief Destroys the adapter. A window left fullscreen is first returned through LeaveFullscreen with
 *        faults disabled, because destroy refuses to free an adapter that still holds saved state.
 */
static int teardown_adapter(window_state_adapter_t *adapter, wsa_double_t *d) {
    waddle_window_state_action_t leave;
    uint64_t completed = 0;
    window_state_adapter_status_t status;
    memset(&leave, 0, sizeof(leave));
    leave.kind = WaddleWindowStateActionGuestLeaveFullscreen;
    status = window_state_adapter_destroy(&adapter);
    if (status == WindowStateAdapterBusy) {
        d->fault_per_mille = 0;
        Check(window_state_adapter_execute(adapter, HwndMain, IdMain, GenMain, &leave, 1, &completed) ==
              WindowStateAdapterOk);
        status = window_state_adapter_destroy(&adapter);
    }
    Check(status == WindowStateAdapterOk);
    Check(adapter == NULL);
    return 0;
}

/** @brief One randomized round. Returns 0 on success. */
static int run_round(uint32_t round, uint64_t *guest_completions, uint64_t *native_failures, uint64_t *stale_recoveries) {
    wsa_double_t d;
    window_state_adapter_t *adapter = NULL;
    waddle_window_state_handle_t *handle = NULL;
    window_state_rect_t rect = {100, 100, 500, 400};
    uint32_t state = 0;

    wsa_double_init(&d, OwnerThread);
    Check(wsa_double_add(&d, HwndMain, 7, 9, rect) != NULL);
    d.rng = UINT64_C(0x2545f4914f6cdd1d) ^ (uint64_t)(round + 1u);
    Check(window_state_adapter_create(wsa_double_api(), &d, &adapter) == WindowStateAdapterOk);
    Check(window_state_adapter_admit(adapter, HwndMain, IdMain, GenMain) == WindowStateAdapterOk);

    /* Initial synchronization: the first observation creates the machine and reconciles it. */
    Check(observe_clean(adapter, &d, &state) == WindowStateAdapterOk);
    Check(waddle_window_state_create(IdMain, GenMain, state, &handle) == WaddleWindowStateOk);
    Check(resynchronize(handle, adapter, &d) == 0);
    Check(check_converged(handle, adapter, &d) == 0);

    for (uint32_t step = 0; step < 80; ++step) {
        waddle_window_state_input_t input;
        waddle_window_state_plan_t plan;
        waddle_window_state_snapshot_t snapshot;
        window_state_adapter_status_t status;
        uint64_t completed = 0;
        int ok_success = 0;
        int plan_status;

        memset(&input, 0, sizeof(input));
        switch (draw(10)) {
        case 0:
        case 1:
            input.kind = WaddleWindowStateInputGuestObservation;
            Check(observe_clean(adapter, &d, &state) == WindowStateAdapterOk);
            input.state = state;
            break;
        case 2:
        case 3:
        case 4:
            input.kind = WaddleWindowStateInputHostConfigure;
            input.state = draw(16);
            input.x = (int32_t)draw(300) - 50;
            input.y = (int32_t)draw(300) - 50;
            input.width = draw(5) == 0 ? 0u : 1u + draw(900);
            input.height = input.width == 0 ? 0u : 1u + draw(700);
            break;
        case 5:
            input.kind = WaddleWindowStateInputHostStateRequest;
            input.maximize = draw(2);
            break;
        case 6:
        case 7:
            input.kind = WaddleWindowStateInputCloseRequest;
            break;
        case 8:
            input.kind = WaddleWindowStateInputHideObserved;
            break;
        default:
            input.kind = WaddleWindowStateInputDestroyObserved;
            break;
        }

        plan_status = waddle_window_state_plan(handle, IdMain, GenMain, &input, &plan);
        if (plan_status != WaddleWindowStateOk) {
            /* Refused by the reducer: the only legal recovery is a fresh observation. */
            Check(plan_status == WaddleWindowStateUnsynchronized || plan_status == WaddleWindowStateUnsupportedStateRequest ||
                  plan_status == WaddleWindowStateRetired || plan_status == WaddleWindowStateInvalidState ||
                  plan_status == WaddleWindowStateInvalidGeometry);
            if (plan_status == WaddleWindowStateRetired) break;
            if (plan_status == WaddleWindowStateUnsynchronized) Check(resynchronize(handle, adapter, &d) == 0);
            Check(check_converged(handle, adapter, &d) == 0);
            continue;
        }

        {
            int has_guest = 0;
            int has_host = 0;
            for (uint64_t index = 0; index < plan.action_count; ++index) {
                if (is_guest_kind(plan.actions[index].kind)) has_guest = 1;
                else has_host = 1;
            }
            /* The reducer never mixes host and guest actions in one plan; the adapter relies on it. */
            Check(!(has_guest && has_host));

            if (has_guest) {
                d.fault_per_mille = 150;
                status = window_state_adapter_execute(adapter, HwndMain, IdMain, GenMain, plan.actions,
                                                      plan.action_count, &completed);
                d.fault_per_mille = 0;
                if (status == WindowStateAdapterOk) {
                    Check(completed == plan.action_count);
                    ok_success = 1;
                    *guest_completions += completed;
                } else {
                    Check(status == WindowStateAdapterNativeFailure || status == WindowStateAdapterStaleHandle);
                    Check(completed < plan.action_count);
                    if (status == WindowStateAdapterNativeFailure) *native_failures += 1u;
                    if (status == WindowStateAdapterStaleHandle) {
                        /* An injected fault on the identity query looks like a vanished window. The
                         * window still exists, so admission succeeds again and the machine resynchronizes. */
                        Check(window_state_adapter_admit(adapter, HwndMain, IdMain, GenMain) == WindowStateAdapterOk);
                        *stale_recoveries += 1u;
                    }
                }
            } else {
                /* Host-side plans: the compositor is assumed to have applied them. */
                ok_success = 1;
            }

            if (ok_success) {
                Check(waddle_window_state_complete(handle, &plan, 1, plan.action_count) == WaddleWindowStateOk);
            } else {
                Check(waddle_window_state_complete(handle, &plan, 0, completed) == WaddleWindowStateOk);
                Check(resynchronize(handle, adapter, &d) == 0);
            }
        }

        Check(waddle_window_state_snapshot(handle, &snapshot) == WaddleWindowStateOk);
        if (snapshot.retired != 0) break;
        Check(check_converged(handle, adapter, &d) == 0);
    }

    Check(waddle_window_state_destroy(&handle) == WaddleWindowStateOk);
    Check(handle == NULL);
    Check(teardown_adapter(adapter, &d) == 0);
    return 0;
}

int main(void) {
    uint64_t guest_completions = 0;
    uint64_t native_failures = 0;
    uint64_t stale_recoveries = 0;
    uint32_t rounds = 60;
    for (uint32_t round = 0; round < rounds; ++round) {
        if (run_round(round, &guest_completions, &native_failures, &stale_recoveries) != 0) {
            fprintf(stderr, "FAILED: round %u\n", round);
            return 1;
        }
    }
    printf("guest actions completed: %llu, native failures: %llu, stale recoveries: %llu\n",
           (unsigned long long)guest_completions, (unsigned long long)native_failures,
           (unsigned long long)stale_recoveries);
    if (guest_completions < 200u || native_failures == 0u) {
        fprintf(stderr, "integration: the run did not exercise successes and faults\n");
        return 1;
    }
    printf("window_state_adapter_integration: all tests passed\n");
    return 0;
}
