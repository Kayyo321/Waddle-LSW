#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_control_windows.h"
#include "session_control_windows_api_double.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_preflight(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    union { max_align_t alignment; unsigned char bytes[sizeof(config) + sizeof(owner) + 16]; } scratch;
    memset(&scratch, 0, sizeof(scratch));
    config.connection_token = 9;
    assert(interop_guest_session_run(NULL, &owner) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, NULL) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run((const interop_guest_session_config_t *)(uintptr_t)(UINTPTR_MAX - 3), &owner) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run((const interop_guest_session_config_t *)(void *)(scratch.bytes + 1), &owner) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, (interop_guest_session_owner_t **)(void *)(scratch.bytes + 1)) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, (interop_guest_session_owner_t **)(uintptr_t)(UINTPTR_MAX - 3)) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, (interop_guest_session_owner_t **)(void *)&config) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_retry_cleanup((interop_guest_session_owner_t **)(uintptr_t)(UINTPTR_MAX - 3), 0) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_retry_cleanup((interop_guest_session_owner_t **)(void *)(scratch.bytes + 1), 0) == InteropGuestSessionInvalidArgument);
    *(interop_guest_session_owner_t **)(void *)scratch.bytes = (interop_guest_session_owner_t *)(uintptr_t)1;
    assert(interop_guest_session_run(&config, (interop_guest_session_owner_t **)(void *)scratch.bytes) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionFreshAuthorizationRequired);
    assert(owner == NULL && interop_guest_test_wsa_calls() == 0);
    config.fresh_exclusive_authorized = 2;
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionInvalidArgument);
    assert(owner == NULL && interop_guest_test_wsa_calls() == 0);
    config.fresh_exclusive_authorized = 1;
    config.connection_token = 0;
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionInvalidArgument);
    assert(owner == NULL && interop_guest_test_wsa_calls() == 0);
    config.connection_token = 9;
    config.reserved_zero = 1;
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionInvalidArgument);
    assert(owner == NULL && interop_guest_test_wsa_calls() == 0);
}
static void test_private_range_and_timer_helpers(void) {
    uint32_t valid = 9, overlap = 9, timeout = 9;
    char bytes[16] = {0};
    assert(interop_guest_test_is_owner_thread_null() == 0);
    assert(interop_guest_test_session_helpers(bytes, 1, 1, bytes, 1, bytes + 8, 1,
        0, 1, NULL, &overlap, &timeout) == 0);
    assert(interop_guest_test_session_helpers(bytes, 1, 1, bytes, 1, bytes + 8, 1,
        0, 1, &valid, NULL, &timeout) == 0);
    assert(interop_guest_test_session_helpers(bytes, 1, 1, bytes, 1, bytes + 8, 1,
        0, 1, &valid, &overlap, NULL) == 0);
#define Probe(pointer_value, length_value, alignment_value, left_value, left_length_value, right_value, right_length_value, now_value, deadline_value, expected_range, expected_overlap, expected_timeout) \
    do { assert(interop_guest_test_session_helpers((pointer_value), (length_value), (alignment_value), \
        (left_value), (left_length_value), (right_value), (right_length_value), (now_value), (deadline_value), \
        &valid, &overlap, &timeout) == 1); \
        assert(valid == (expected_range)); assert(overlap == (expected_overlap)); assert(timeout == (expected_timeout)); } while (0)
    Probe(NULL, 1, 1, bytes, 0, bytes + 8, 1, 1, 1, 0, 1, 0);
    Probe(bytes, 0, 1, bytes, 1, bytes + 8, 0, 1, 50, 0, 1, 49);
    Probe(bytes, 1, 0, (const void *)(uintptr_t)(UINTPTR_MAX - 3), 8,
          bytes, 1, 1, 101, 0, 1, 100);
    Probe(bytes + 1, 1, 2, bytes, 1, (const void *)(uintptr_t)(UINTPTR_MAX - 3), 8,
          1, 101, 0, 1, 100);
    Probe((const void *)(uintptr_t)(UINTPTR_MAX - 3), 8, 1, bytes, 1, bytes + 8, 1,
          1, 1000, 0, 0, 100);
    Probe(bytes, sizeof(bytes), 1, bytes, sizeof(bytes), bytes + 8, 1, 10, 110, 1, 1, 100);
    Probe(bytes, sizeof(bytes), 1, bytes, sizeof(bytes), bytes + sizeof(bytes), 1,
          20, 1000, 1, 0, 100);
#undef Probe
}
static void test_ready_retained_until_detached(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_state_t state;
    config.device_index = 4;
    config.fresh_exclusive_authorized = 1;
    config.connection_token = 0x1234;
    assert(interop_guest_session_get_state(NULL, &state) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_get_state((interop_guest_session_owner_t *)(uintptr_t)1, NULL) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_retry_cleanup(NULL, 0) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_retry_cleanup(&owner, 0) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_retry_cleanup(&owner, 2) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    assert(interop_guest_test_wsa_calls() == 1);
    assert(interop_guest_test_socket_drive_calls() > 1);
    assert(interop_guest_test_map_close_calls() == 1);
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
    assert(interop_guest_session_get_state(owner, (interop_guest_session_state_t *)(void *)owner) == InteropGuestSessionInvalidArgument);
    assert(state.winsock_started == 0);
    assert(state.family_handle_owned == 0 && state.listener_owned == 0);
    assert(state.accepted_socket_owned == 0 && state.socket_owner_owned == 0);
    assert(state.mapping_owner_owned == 1 && state.mapping_view_published == 1);
    assert(state.operational_result == InteropGuestSessionTimedOut);
    interop_guest_test_set_thread_id(78);
    memset(&state, 0xa5, sizeof(state));
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionWrongThread);
    assert(((const unsigned char *)&state)[0] == 0xa5);
    assert(interop_guest_session_retry_cleanup(&owner, 0) == InteropGuestSessionWrongThread);
    interop_guest_test_set_thread_id(77);
    assert(interop_guest_session_retry_cleanup(&owner, 0) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL && interop_guest_test_map_close_calls() == 2);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL && interop_guest_test_map_close_calls() == 3);
}
static interop_guest_session_result_t run_stage(interop_guest_test_failure_t failure,
    HANDLE stop_event, interop_guest_session_owner_t **owner) {
    interop_guest_session_config_t config = {0};
    config.device_index = 4;
    config.fresh_exclusive_authorized = 1;
    config.connection_token = 0x55;
    config.stop_event = stop_event;
    interop_guest_test_reset();
    interop_guest_test_set_failure(failure);
    return interop_guest_session_run(&config, owner);
}
static void test_failure_stages(void) {
    const struct {
        interop_guest_test_failure_t failure;
        interop_guest_session_result_t expected;
        HANDLE stop_event;
    } cases[] = {
        {InteropGuestTestFailStartup, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailSessionAllocation, InteropGuestSessionOutOfMemory, 0},
        {InteropGuestTestClockError, InteropGuestSessionOsError, 0},
        {InteropGuestTestClockNearWrap, InteropGuestSessionOsError, 0},
        {InteropGuestTestNoReadyTimeout, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestEofBeforeReady, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestRunTimeoutAfterReady, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestZeroEpoch, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestMaxHeartbeat, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestShortPollTimeout, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestSocketWorkClosePending, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestSocketWorkCloseIndeterminate, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestSocketWorkPeerFailure, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailHeartbeatLoadDuringRun, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailOpenFamily, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailQueryFamily, InteropGuestSessionOsError, 0},
        {InteropGuestTestBadFamily, InteropGuestSessionOsError, 0},
        {InteropGuestTestBadFamilySize, InteropGuestSessionOsError, 0},
        {InteropGuestTestZeroFamily, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailCloseHandle, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailWsaCleanup, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailSocketClose, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestPeerQueryAndCloseFailure, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailCreateListener, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailListenerInherit, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailListenerNonblock, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailBind, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailListen, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailListenerPoll, InteropGuestSessionOsError, 0},
        {InteropGuestTestBadPollEvent, InteropGuestSessionOsError, 0},
        {InteropGuestTestListenerNoEvent, InteropGuestSessionTimedOut, 0},
        {InteropGuestTestNoIncomingPeer, InteropGuestSessionTimedOut, 0},
        {InteropGuestTestFailAccept, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailAcceptedInherit, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailAcceptedNonblock, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailPeerQuery, InteropGuestSessionOsError, 0},
        {InteropGuestTestBadPeer, InteropGuestSessionRejectedPeer, 0},
        {InteropGuestTestBadPeerLength, InteropGuestSessionRejectedPeer, 0},
        {InteropGuestTestBadPeerFamily, InteropGuestSessionRejectedPeer, 0},
        {InteropGuestTestBadPeerReserved, InteropGuestSessionRejectedPeer, 0},
        {InteropGuestTestFailSocketCreate, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailMappingCreate, InteropGuestSessionMappingFailed, 0},
        {InteropGuestTestFailMappingOpen, InteropGuestSessionMappingFailed, 0},
        {InteropGuestTestFailRandom, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailMappingDestroy, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailMappingInitialize, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailMappingResult, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailSocketPoll, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailSessionPoll, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailSocketDrive, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailGuestHeartbeatLoad, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailHostHeartbeatLoad, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFailGuestHeartbeatStore, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestReadyCapabilities, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestHeaderInvalid, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestFeatureEvent, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestDrainingEvent, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestErrorEvent, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestNonreadyPhase, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestImmediateWork, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestImmediateWork, InteropGuestSessionCleanupRequired, (HANDLE)1},
        {InteropGuestTestHeartbeatMismatch, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestStop, InteropGuestSessionStopped, 1},
        {InteropGuestTestStopDuringRun, InteropGuestSessionCleanupRequired, 1},
        {InteropGuestTestFailStopWaitDuringRun, InteropGuestSessionCleanupRequired, 1},
        {InteropGuestTestStopAfterSessionPoll, InteropGuestSessionCleanupRequired, 1},
        {InteropGuestTestFailStopWaitAfterSessionPoll, InteropGuestSessionCleanupRequired, 1},
        {InteropGuestTestNoFailure, InteropGuestSessionCleanupRequired, 2},
        {InteropGuestTestFailStopWait, InteropGuestSessionOsError, 1},
        {InteropGuestTestClockBackwards, InteropGuestSessionOsError, 0},
        {InteropGuestTestFailSocketClosePending, InteropGuestSessionCleanupRequired, 0},
        {InteropGuestTestRetrySocketClosePending, InteropGuestSessionCleanupRequired, 0}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        interop_guest_session_owner_t *owner = NULL;
        interop_guest_session_result_t result = run_stage(cases[index].failure,
            cases[index].stop_event, &owner);
        if (result != cases[index].expected) { fprintf(stderr, "failure stage %d returned %d expected %d\n", cases[index].failure, result, cases[index].expected); }
        assert(result == cases[index].expected);
        if (owner != NULL) {
            interop_guest_test_set_failure(InteropGuestTestNoFailure);
            interop_guest_session_result_t cleanup_result = interop_guest_session_retry_cleanup(&owner, 1);
            if (cleanup_result == InteropGuestSessionCleanupRequired) {
                interop_guest_session_quarantine_t quarantine = {0};
                assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionSuccess);
                assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionSuccess);
            } else { assert(cleanup_result == InteropGuestSessionSuccess); }
            assert(owner == NULL);
        }
    }
}
static void test_native_direct_error_diagnostics(void) {
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_state_t state;

    assert(run_stage(InteropGuestTestFailStartup, 0, &owner) == InteropGuestSessionOsError);
    assert(owner == NULL && interop_guest_test_last_wsa_error_calls() == 0);

    assert(run_stage(InteropGuestTestFailRandom, 0, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
    assert(state.last_ntstatus == UINT32_C(0xc000000d) && state.last_win32_error == 0);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);

    assert(run_stage(InteropGuestTestZeroEpoch, 0, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
    assert(state.last_ntstatus == 0 && state.last_win32_error == ERROR_INVALID_DATA);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);

    assert(run_stage(InteropGuestTestBadFamilyCloseFailure, 0, &owner) ==
        InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
    assert(state.last_win32_error == ERROR_INVALID_DATA);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 0) == InteropGuestSessionSuccess);
    assert(owner == NULL);
}
static void test_heartbeat_and_cleanup_retries(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0x88;
    interop_guest_test_reset();
    interop_guest_test_set_fast_timeout(0);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL && interop_guest_test_final_guest_heartbeat() > 1);
    interop_guest_test_set_failure(InteropGuestTestMapClose);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);

    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestSocketClosePending);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);
}
static void test_pre_ready_terminal_reasons(void) {
    const struct { interop_guest_test_failure_t failure; uint32_t expected; } cases[] = {
        {InteropGuestTestNoReadyTimeout, InteropGuestSessionTimedOut},
        {InteropGuestTestEofBeforeReady, InteropGuestSessionPeerFailed},
        {InteropGuestTestRunTimeoutAfterReady, InteropGuestSessionTimedOut}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        interop_guest_session_owner_t *owner = NULL;
        interop_guest_session_state_t state;
        assert(run_stage(cases[index].failure, 0, &owner) == InteropGuestSessionCleanupRequired);
        assert(owner != NULL);
        assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
        assert(state.operational_result == cases[index].expected);
        assert(state.mapping_view_published == 1);
        assert(interop_guest_test_final_guest_heartbeat() == 0);
        assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
        assert(owner == NULL);
    }
}
static void test_clock_failures_at_each_stage(void) {
    for (uint32_t read_number = 1; read_number <= 14; read_number++) {
        interop_guest_session_config_t config = {0};
        interop_guest_session_owner_t *owner = NULL;
        interop_guest_session_state_t state;
        config.device_index = 4;
        config.fresh_exclusive_authorized = 1;
        config.connection_token = UINT64_C(0x7000) + read_number;
        interop_guest_test_reset();
        interop_guest_test_set_failure(InteropGuestTestNoFailure);
        interop_guest_test_fail_clock_read(read_number);
        interop_guest_session_result_t result = interop_guest_session_run(&config, &owner);
        assert(result != InteropGuestSessionSuccess);
        if (owner != NULL) {
            assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
            assert(state.operational_result == InteropGuestSessionOsError ||
                   state.operational_result == InteropGuestSessionTimedOut);
            assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
            assert(owner == NULL);
        } else {
            assert(result == InteropGuestSessionOsError);
        }
    }
}
static void test_clock_deadline_overflow_boundaries(void) {
    const struct { uint32_t read_number; uint64_t time_ms; uint32_t expected; } cases[] = {
        {5, UINT64_MAX - 100, InteropGuestSessionOsError},
        {7, UINT64_MAX - 100, InteropGuestSessionTimedOut},
        {10, UINT64_MAX - 100, InteropGuestSessionOsError}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        interop_guest_session_config_t config = {0};
        interop_guest_session_owner_t *owner = NULL;
        interop_guest_session_state_t state;
        config.device_index = 4; config.fresh_exclusive_authorized = 1;
        config.connection_token = UINT64_C(0x8000) + index;
        interop_guest_test_reset();
        interop_guest_test_set_failure(InteropGuestTestNoFailure);
        interop_guest_test_override_clock_read(cases[index].read_number, cases[index].time_ms);
        interop_guest_session_result_t result = interop_guest_session_run(&config, &owner);
        assert(result == InteropGuestSessionCleanupRequired || result == InteropGuestSessionOsError);
        if (owner != NULL) {
            assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
            assert(state.operational_result == cases[index].expected);
            if (cases[index].read_number == 10) { assert(interop_guest_test_final_guest_heartbeat() == 0); }
            assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
        } else { assert(result == InteropGuestSessionOsError); }
    }
}
static void test_indeterminate_socket_quarantine(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_state_t state;
    interop_guest_session_quarantine_t quarantine = {0};
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0x99;
    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestSocketCloseIndeterminate);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionCleanupRequired);
    assert(interop_guest_session_get_state(owner, &state) == InteropGuestSessionSuccess);
    assert(state.mapping_owner_owned == 0 && state.socket_owner_owned == 1);
    assert(interop_guest_session_quarantine_socket((interop_guest_session_owner_t **)(void *)((unsigned char *)&owner + 1), &quarantine) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_quarantine_socket(&owner, (interop_guest_session_quarantine_t *)(uintptr_t)(UINTPTR_MAX - 3)) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_quarantine_socket(&owner, (interop_guest_session_quarantine_t *)(void *)((unsigned char *)&quarantine + 1)) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_quarantine_socket(&owner, (interop_guest_session_quarantine_t *)(void *)&owner) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_quarantine_socket((interop_guest_session_owner_t **)(void *)&quarantine, &quarantine) == InteropGuestSessionInvalidArgument);
    interop_guest_test_set_thread_id(78);
    assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionWrongThread);
    interop_guest_test_set_thread_id(77);
    quarantine.sockets[0].socket_value = 3;
    assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionInvalidArgument);
    assert(owner != NULL && quarantine.sockets[0].socket_value == 3);
    memset(&quarantine, 0, sizeof(quarantine));
    assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionSuccess);
    assert(owner == NULL && quarantine.socket_count == 1 && quarantine.winsock_reference_owned == 1);
    assert(quarantine.sockets[0].socket_value == 11 && quarantine.sockets[0].connection_token == 0x99);
    assert(interop_guest_session_release_quarantine(&quarantine, 0) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_release_quarantine(NULL, 1) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_release_quarantine((interop_guest_session_quarantine_t *)(uintptr_t)(UINTPTR_MAX - 3), 1) == InteropGuestSessionInvalidArgument);
    assert(interop_guest_session_release_quarantine((interop_guest_session_quarantine_t *)(void *)((unsigned char *)&quarantine + 1), 1) == InteropGuestSessionInvalidArgument);
    interop_guest_session_quarantine_t invalid = quarantine;
    invalid.socket_count = 0;
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    invalid = quarantine; invalid.sockets[0].socket_value = 0;
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    invalid = quarantine; invalid.sockets[0].close_classification = 99;
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    invalid = quarantine; invalid.sockets[1].socket_value = 99;
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    invalid = quarantine; invalid.socket_count = 2; invalid.sockets[1] = invalid.sockets[0];
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    invalid = quarantine; invalid.socket_count = 2; invalid.sockets[1] = invalid.sockets[0]; invalid.sockets[1].socket_value++;
    invalid.sockets[1].owner_thread_id++;
    assert(interop_guest_session_release_quarantine(&invalid, 1) == InteropGuestSessionInvalidArgument);
    interop_guest_test_set_thread_id(78);
    assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionWrongThread);
    interop_guest_test_set_thread_id(77);
    interop_guest_test_set_failure(InteropGuestTestFailWsaCleanup);
    assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionOsError);
    assert(quarantine.socket_count == 1 && quarantine.winsock_reference_owned == 1 && quarantine.last_wsa_error == 5);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionSuccess);
    assert(quarantine.socket_count == 0 && quarantine.winsock_reference_owned == 0);
}
static void test_indeterminate_raw_socket_quarantine_never_retries(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_quarantine_t quarantine = {0};
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0xa1;
    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestFailSocketClose);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    uint32_t close_attempts = interop_guest_test_close_socket_calls();
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionCleanupRequired);
    assert(interop_guest_test_close_socket_calls() == close_attempts);
    assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionSuccess);
    assert(owner == NULL && quarantine.socket_count == 1 &&
           quarantine.sockets[0].close_classification == InteropWindowsSocketCloseIndeterminateState);
    assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionSuccess);
}
static void test_pending_raw_socket_bundle_and_winsock_lifetime(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    interop_guest_session_quarantine_t quarantine = {0};
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0xa3;
    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestPeerQueryAndCloseFailure);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    uint32_t initial_closes = interop_guest_test_close_socket_calls();
    assert(initial_closes == 2);
    assert(interop_guest_session_retry_cleanup(&owner, 0) == InteropGuestSessionCleanupRequired);
    assert(interop_guest_test_close_socket_calls() == initial_closes + 2);
    assert(interop_guest_session_quarantine_socket(&owner, &quarantine) == InteropGuestSessionSuccess);
    assert(owner == NULL && quarantine.socket_count == 2 && quarantine.winsock_reference_owned == 1);
    assert(quarantine.sockets[0].close_classification == InteropWindowsSocketClosePendingState);
    assert(quarantine.sockets[1].close_classification == InteropWindowsSocketClosePendingState);
    assert(interop_guest_session_release_quarantine(&quarantine, 1) == InteropGuestSessionSuccess);
}
static void test_peer_destroy_failure_retry(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0xa2;
    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestSocketDestroyPeerFailure);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);
}
static void test_mapping_destroy_failure_retry(void) {
    interop_guest_session_config_t config = {0};
    interop_guest_session_owner_t *owner = NULL;
    config.device_index = 4; config.fresh_exclusive_authorized = 1; config.connection_token = 0xa4;
    interop_guest_test_reset();
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_run(&config, &owner) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    interop_guest_test_set_failure(InteropGuestTestFailMappingDestroy);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionCleanupRequired);
    assert(owner != NULL);
    interop_guest_test_set_failure(InteropGuestTestNoFailure);
    assert(interop_guest_session_retry_cleanup(&owner, 1) == InteropGuestSessionSuccess);
    assert(owner == NULL);
}
int main(void) {
    interop_guest_test_reset();
    test_preflight();
    test_private_range_and_timer_helpers();
    interop_guest_test_reset();
    test_ready_retained_until_detached();
    test_failure_stages();
    test_native_direct_error_diagnostics();
    test_heartbeat_and_cleanup_retries();
    test_pre_ready_terminal_reasons();
    test_clock_failures_at_each_stage();
    test_clock_deadline_overflow_boundaries();
    test_indeterminate_socket_quarantine();
    test_indeterminate_raw_socket_quarantine_never_retries();
    test_pending_raw_socket_bundle_and_winsock_lifetime();
    test_peer_destroy_failure_retry();
    test_mapping_destroy_failure_retry();
    puts("session_control_windows tests passed");
    return 0;
}
