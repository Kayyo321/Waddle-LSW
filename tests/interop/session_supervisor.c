/*
 * Supervisor unit tests: scripted attempts through fake runner, wait, stop and cleanup seams.
 * No socket, daemon or guest is involved; the integration test lives in session_app.c.
 */
#include "../../src/interop/session_supervisor.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ScriptCapacity 64
#define WaitCapacity 512

typedef struct script_t {
    interop_host_control_result_t outcome[ScriptCapacity];
    uint32_t admission[ScriptCapacity];
    uint8_t reached_ready[ScriptCapacity];
    uint8_t guest_error[ScriptCapacity];
    uint64_t lifetime_ms[ScriptCapacity];
    uint64_t ended_at[ScriptCapacity]; /* explicit end time for a Ready attempt; 0 uses ready + lifetime */
    int wait_error_at; /* wait index that reports an OS failure; -1 none */
    uint32_t count; /* scripted attempts; attempts past the script end as terminal success */
    uint32_t runs;
    uint32_t cleanup_calls;
    uint32_t cleanup_success_after; /* cleanup call index that succeeds; 0 never succeeds */
    uint32_t waits[WaitCapacity];
    uint32_t wait_count;
    int stop_at_wait; /* wait index that reports stop; -1 none */
    uint32_t stop_after_runs; /* stop requested once runs reaches this value; 0 with stop_now set */
    bool stop_now;
    bool owners_zero_on_entry;
} script_t;

/* Stand-in for a published session owner; the supervisor only forwards it, never dereferences it. */
static char dummy_session_owner[1];

static int script_wait(void *context, uint32_t ms, int stop_fd)
{
    script_t *script = context;
    (void)stop_fd;
    assert(script->wait_count < WaitCapacity);
    script->waits[script->wait_count] = ms;
    if ((int)script->wait_count == script->wait_error_at) {
        ++script->wait_count;
        return -1;
    }
    if ((int)script->wait_count == script->stop_at_wait) {
        ++script->wait_count;
        return 1;
    }
    ++script->wait_count;
    return 0;
}

static int script_stop_requested(void *context, int stop_fd)
{
    script_t *script = context;
    (void)stop_fd;
    if (script->stop_now) return 1;
    return script->stop_after_runs != 0 && script->runs >= script->stop_after_runs;
}

static interop_host_control_result_t script_run(void *context, const session_app_config_t *app,
    interop_host_control_owners_t *owners, interop_host_control_detail_t *detail)
{
    script_t *script = context;
    uint32_t index = script->runs++;

    (void)app;
    if (owners->session_owner != NULL || owners->mapping_owner != NULL) script->owners_zero_on_entry = false;
    if (index >= script->count) return HostControlSuccess;
    detail->admission_result = script->admission[index];
    detail->reached_ready = script->reached_ready[index];
    detail->guest_error = script->guest_error[index];
    if (detail->reached_ready) {
        detail->ready_at_ms = 1000;
        detail->ended_at_ms = script->ended_at[index] != 0 ? script->ended_at[index]
            : 1000 + script->lifetime_ms[index];
    }
    if (script->outcome[index] == HostControlCleanupRequired) {
        owners->session_owner = (interop_linux_session_owner_t *)(void *)dummy_session_owner;
    }
    return script->outcome[index];
}

static interop_host_control_result_t script_retry_cleanup(void *context, interop_host_control_owners_t *owners)
{
    script_t *script = context;
    ++script->cleanup_calls;
    if (script->cleanup_success_after != 0 && script->cleanup_calls >= script->cleanup_success_after) {
        owners->session_owner = NULL;
        owners->mapping_owner = NULL;
        return HostControlSuccess;
    }
    return HostControlCleanupRequired;
}

static session_supervisor_seams_t script_seams(script_t *script)
{
    session_supervisor_seams_t seams;
    memset(&seams, 0, sizeof(seams));
    seams.context = script;
    seams.wait_ms = script_wait;
    seams.stop_requested = script_stop_requested;
    seams.run = script_run;
    seams.retry_cleanup = script_retry_cleanup;
    return seams;
}

static void script_reset(script_t *script)
{
    memset(script, 0, sizeof(*script));
    script->stop_at_wait = -1;
    script->wait_error_at = -1;
    script->owners_zero_on_entry = true;
}

static void script_add(script_t *script, interop_host_control_result_t outcome, uint32_t admission,
    uint8_t ready, uint8_t guest_error, uint64_t lifetime)
{
    uint32_t index = script->count++;
    assert(index < ScriptCapacity);
    script->outcome[index] = outcome;
    script->admission[index] = admission;
    script->reached_ready[index] = ready;
    script->guest_error[index] = guest_error;
    script->lifetime_ms[index] = lifetime;
}

static session_supervisor_config_t supervised_config(script_t *script, uint32_t max_retries)
{
    session_supervisor_config_t config;
    memset(&config, 0, sizeof(config));
    config.app.control.stop_fd = 3;
    config.seams = script_seams(script);
    config.max_retries = max_retries;
    return config;
}

static interop_host_control_owners_t *fresh_owners(interop_host_control_owners_t *owners)
{
    memset(owners, 0, sizeof(*owners));
    return owners;
}

static void test_classification(void)
{
    interop_host_control_detail_t detail;
    memset(&detail, 0, sizeof(detail));

    /* Terminal outcomes regardless of detail. */
    assert(session_supervisor_classify(HostControlSuccess, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlCancelled, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlInvalidArgument, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlFeatureFailed, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlUnexpectedFeature, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlMappingFailed, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlOsError, NULL) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlCleanupRequired, NULL) == SessionSupervisorTerminal);

    /* Admission refusals: daemon, guest and connector conditions retry; contention and consumption end. */
    detail.admission_result = HostSessionNotRunning;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorRetry);
    detail.admission_result = HostSessionGuestUnavailable;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorRetry);
    detail.admission_result = HostSessionConnectError;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorRetry);
    detail.admission_result = HostSessionDaemonError;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorRetry);
    detail.admission_result = HostSessionTimedOut;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorRetry);
    detail.admission_result = HostSessionBusy;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorTerminal);
    detail.admission_result = HostSessionAttemptConsumed;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorTerminal);
    detail.admission_result = HostSessionPeerCreateError;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorTerminal);
    detail.admission_result = HostSessionCancelled;
    assert(session_supervisor_classify(HostControlAdmissionFailed, &detail) == SessionSupervisorTerminal);

    /* Peer loss: a dropped guest retries; a guest protocol error ends the supervisor. */
    memset(&detail, 0, sizeof(detail));
    detail.reached_ready = 1;
    assert(session_supervisor_classify(HostControlPeerFailed, &detail) == SessionSupervisorRetry);
    detail.guest_error = 1;
    assert(session_supervisor_classify(HostControlPeerFailed, &detail) == SessionSupervisorTerminal);
    assert(session_supervisor_classify(HostControlDaemonLost, NULL) == SessionSupervisorRetry);
    assert(session_supervisor_classify(HostControlTimedOut, NULL) == SessionSupervisorRetry);
}

static void test_backoff_schedule(void)
{
    script_t script;
    interop_host_control_owners_t owners;
    session_supervisor_report_t report;
    session_supervisor_config_t config;
    static const uint32_t expected[] = { 250, 500, 1000, 2000, 4000, 4000, 4000 };
    size_t index;

    script_reset(&script);
    for (index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index)
        script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlSuccess);
    assert(report.attempts == 8 && report.retries == 7);
    assert(script.wait_count == 7);
    for (index = 0; index < 7; ++index) assert(script.waits[index] == expected[index]);
    assert(report.backoff_total_ms == 15750);
    assert(report.ready_sessions == 0 && report.cleanup_retries == 0);
}

static void test_stable_session_resets_backoff(void)
{
    script_t script;
    interop_host_control_owners_t owners;
    session_supervisor_report_t report;
    session_supervisor_config_t config;

    /* Attempts 0 and 1 are unstable, attempt 2 is stable: the stable one resets the delay before waiting. */
    script_reset(&script);
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    script_add(&script, HostControlPeerFailed, 0, 1, 0, SessionSupervisorStableMs - 1);
    script_add(&script, HostControlPeerFailed, 0, 1, 0, SessionSupervisorStableMs);
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlSuccess);
    /* Attempt 1 (unstable Ready) keeps doubling; attempt 2 (stable Ready) restarts the wait at 250 ms. */
    assert(script.wait_count == 4);
    assert(script.waits[0] == 250);
    assert(script.waits[1] == 500);
    assert(script.waits[2] == 250);
    assert(script.waits[3] == 500);
    assert(report.ready_sessions == 2);
}

static void test_stop_paths(void)
{
    script_t script;
    interop_host_control_owners_t owners;
    session_supervisor_report_t report;
    session_supervisor_config_t config;

    /* An OS failure while waiting ends the supervisor as OsError without another attempt. */
    script_reset(&script);
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    script_add(&script, HostControlSuccess, 0, 0, 0, 0);
    script.wait_error_at = 0;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlOsError);
    assert(report.attempts == 1 && script.runs == 1);

    /* An end time before Ready is never a stable session, so the delay keeps doubling. */
    script_reset(&script);
    script_add(&script, HostControlPeerFailed, 0, 1, 0, 0);
    script.ended_at[0] = 500;
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    script_add(&script, HostControlSuccess, 0, 0, 0, 0);
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlSuccess);
    assert(script.waits[0] == 250 && script.waits[1] == 500);

    /* Stop during backoff ends the supervisor without another attempt. */
    script_reset(&script);
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    script_add(&script, HostControlSuccess, 0, 0, 0, 0);
    script.stop_at_wait = 0;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlCancelled);
    assert(report.attempts == 1 && script.runs == 1);
    assert(report.final_outcome == HostControlCancelled);

    /* Stop before the first attempt runs nothing. */
    script_reset(&script);
    script_add(&script, HostControlSuccess, 0, 0, 0, 0);
    script.stop_now = true;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlCancelled);
    assert(report.attempts == 0 && script.runs == 0 && script.wait_count == 0);

    /* Stop observed after a retryable failure skips the wait entirely. */
    script_reset(&script);
    script_add(&script, HostControlPeerFailed, 0, 0, 0, 0);
    script_add(&script, HostControlSuccess, 0, 0, 0, 0);
    script.stop_after_runs = 1;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlCancelled);
    assert(report.attempts == 1 && script.wait_count == 0);
}

static void test_terminal_and_limits(void)
{
    script_t script;
    interop_host_control_owners_t owners;
    session_supervisor_report_t report;
    session_supervisor_config_t config;
    uint32_t index;

    /* A contended admission ends after one attempt with its own outcome. */
    script_reset(&script);
    script_add(&script, HostControlAdmissionFailed, HostSessionBusy, 0, 0, 0);
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlAdmissionFailed);
    assert(report.attempts == 1 && script.wait_count == 0);

    /* The retry limit returns the last retryable outcome. */
    script_reset(&script);
    for (index = 0; index < 5; ++index) script_add(&script, HostControlDaemonLost, 0, 0, 0, 0);
    config = supervised_config(&script, 2);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlDaemonLost);
    assert(report.attempts == 3 && report.retries == 2);

    /* Cleanup that resolves between attempts is terminal and leaves no owner. */
    script_reset(&script);
    script_add(&script, HostControlCleanupRequired, 0, 0, 0, 0);
    script.cleanup_success_after = 2;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlOsError);
    assert(script.cleanup_calls == 2 && report.cleanup_retries == 2);
    assert(owners.session_owner == NULL && owners.mapping_owner == NULL);

    /* Cleanup that never resolves returns CleanupRequired with the owner still published. */
    script_reset(&script);
    script_add(&script, HostControlCleanupRequired, 0, 0, 0, 0);
    script.cleanup_success_after = 0;
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(&config, fresh_owners(&owners), &report) == HostControlCleanupRequired);
    assert(script.cleanup_calls == 3 && report.cleanup_retries == 3);
    assert(owners.session_owner != NULL);
    owners.session_owner = NULL;
}

static void test_invalid_inputs(void)
{
    script_t script;
    interop_host_control_owners_t owners;
    session_supervisor_report_t report;
    session_supervisor_config_t config;

    script_reset(&script);
    config = supervised_config(&script, 0);
    assert(session_supervisor_run(NULL, fresh_owners(&owners), &report) == HostControlInvalidArgument);
    assert(session_supervisor_run(&config, NULL, &report) == HostControlInvalidArgument);
    assert(report.final_outcome == HostControlInvalidArgument);

    config.app.control.stop_fd = -1;
    assert(session_supervisor_run(&config, fresh_owners(&owners), NULL) == HostControlInvalidArgument);
    config = supervised_config(&script, 0);
    owners = (interop_host_control_owners_t){ 0 };
    owners.mapping_owner = (interop_mapping_linux_t *)(void *)dummy_session_owner;
    assert(session_supervisor_run(&config, &owners, NULL) == HostControlInvalidArgument);
    owners.mapping_owner = NULL;
    config.app.control.stop_fd = 3;

    config.seams.run = NULL;
    assert(session_supervisor_run(&config, fresh_owners(&owners), NULL) == HostControlInvalidArgument);
    config = supervised_config(&script, 0);
    config.seams.wait_ms = NULL;
    assert(session_supervisor_run(&config, fresh_owners(&owners), NULL) == HostControlInvalidArgument);
    config = supervised_config(&script, 0);
    config.seams.retry_cleanup = NULL;
    assert(session_supervisor_run(&config, fresh_owners(&owners), NULL) == HostControlInvalidArgument);
    config = supervised_config(&script, 0);
    config.seams.stop_requested = NULL;
    assert(session_supervisor_run(&config, fresh_owners(&owners), NULL) == HostControlInvalidArgument);

    /* Owners that are not zero on entry are refused before any attempt. */
    config = supervised_config(&script, 0);
    owners.session_owner = (interop_linux_session_owner_t *)(void *)dummy_session_owner;
    assert(session_supervisor_run(&config, &owners, NULL) == HostControlInvalidArgument);
    assert(script.runs == 0);
    owners.session_owner = NULL;
}

/* Deterministic linear congruential generator; the seed fixes every sequence. */
static uint32_t lcg_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void test_random_sequences(void)
{
    static const interop_host_control_result_t Choices[] = {
        HostControlPeerFailed, HostControlDaemonLost, HostControlAdmissionFailed,
        HostControlTimedOut, HostControlSuccess, HostControlFeatureFailed,
        HostControlCleanupRequired, HostControlCancelled,
    };
    uint32_t seed = 0x5eed2026u;
    uint32_t sequence;

    for (sequence = 0; sequence < 2000; ++sequence) {
        script_t script;
        interop_host_control_owners_t owners;
        session_supervisor_report_t report;
        session_supervisor_config_t config;
        uint32_t steps = 1 + lcg_next(&seed) % 12;
        uint32_t step;
        uint32_t stop_at = 0;

        script_reset(&script);
        for (step = 0; step < steps; ++step) {
            interop_host_control_result_t outcome = Choices[lcg_next(&seed) % 8];
            uint32_t admission = lcg_next(&seed) % 2 ? HostSessionNotRunning : HostSessionBusy;
            uint8_t ready = (uint8_t)(lcg_next(&seed) % 2);
            uint64_t lifetime = (uint64_t)(lcg_next(&seed) % 20000);
            uint8_t guest_error = (uint8_t)(lcg_next(&seed) % 4 == 0);
            script_add(&script, outcome, admission, ready, guest_error, lifetime);
        }
        script.cleanup_success_after = 1 + lcg_next(&seed) % 3;
        if (lcg_next(&seed) % 4 == 0) {
            stop_at = 1 + lcg_next(&seed) % steps;
            script.stop_after_runs = stop_at;
        }
        config = supervised_config(&script, lcg_next(&seed) % 3 == 0 ? 1 + lcg_next(&seed) % 4 : 0);
        assert(session_supervisor_run(&config, fresh_owners(&owners), &report) != HostControlInvalidArgument);

        /* Invariants: one run per attempt, one wait per retry, schedule values only, owners zero on entry,
         * no run after a requested stop, and no owner left behind unless cleanup was reported unresolved. */
        assert(report.attempts == script.runs);
        assert(report.retries == script.wait_count);
        assert(script.owners_zero_on_entry);
        assert(owners.session_owner == NULL && owners.mapping_owner == NULL);
        if (stop_at != 0) assert(script.runs <= stop_at);
        for (step = 0; step < script.wait_count; ++step) {
            uint32_t delay = script.waits[step];
            assert(delay == 250 || delay == 500 || delay == 1000 || delay == 2000 || delay == 4000);
            if (step > 0 && delay != SessionSupervisorInitialBackoffMs)
                assert(delay == 2 * script.waits[step - 1] || delay == 4000);
        }
    }
}

static volatile sig_atomic_t interrupts;

static void count_interrupt(int signal_number)
{
    (void)signal_number;
    ++interrupts;
}

static void *interrupt_later(void *context)
{
    struct timespec pause = { 0, 30 * 1000000L };
    (void)context;
    nanosleep(&pause, NULL);
    pthread_kill(*(pthread_t *)context, SIGUSR1);
    return NULL;
}

/* The real seams: poll on a real stop descriptor, interruption by a signal, and the cleanup call. */
static void test_system_seams(void)
{
    session_supervisor_seams_t seams = session_supervisor_system_seams();
    interop_host_control_owners_t owners;
    interop_host_session_config_t admission;
    session_app_config_t app;
    interop_host_control_detail_t detail;
    int stop_pair[2];
    struct sigaction action;
    pthread_t self = pthread_self();
    pthread_t interrupter;

    assert(pipe2(stop_pair, O_NONBLOCK | O_CLOEXEC) == 0);
    assert(seams.stop_requested(NULL, stop_pair[0]) == 0);
    assert(seams.wait_ms(NULL, 20, stop_pair[0]) == 0);
    assert(write(stop_pair[1], "s", 1) == 1);
    assert(seams.stop_requested(NULL, stop_pair[0]) == 1);
    assert(seams.wait_ms(NULL, 1000, stop_pair[0]) == 1);
    assert(close(stop_pair[1]) == 0);
    assert(seams.wait_ms(NULL, 1, -1) == 0); /* poll ignores a negative descriptor: no stop, just the wait */
    assert(close(stop_pair[0]) == 0);
    assert(seams.stop_requested(NULL, stop_pair[0]) == 1); /* a closed descriptor reports POLLNVAL: stop */

    memset(&action, 0, sizeof(action));
    action.sa_handler = count_interrupt;
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(pipe2(stop_pair, O_NONBLOCK | O_CLOEXEC) == 0);
    interrupts = 0;
    assert(pthread_create(&interrupter, NULL, interrupt_later, &self) == 0);
    assert(seams.wait_ms(NULL, 120, stop_pair[0]) == 0);
    assert(pthread_join(interrupter, NULL) == 0);
    assert(interrupts >= 1);
    assert(close(stop_pair[0]) == 0 && close(stop_pair[1]) == 0);

    memset(&owners, 0, sizeof(owners));
    assert(seams.retry_cleanup(NULL, &owners) == HostControlSuccess);

    /* The real runner reports the same invalid-argument outcome as session_app_run_detailed. */
    memset(&app, 0, sizeof(app));
    memset(&admission, 0, sizeof(admission));
    memset(&detail, 0, sizeof(detail));
    app.control.admission = &admission;
    assert(seams.run(NULL, &app, &owners, &detail) == HostControlInvalidArgument);
}

void session_supervisor_unit_tests(void)
{
    test_classification();
    test_backoff_schedule();
    test_stable_session_resets_backoff();
    test_stop_paths();
    test_terminal_and_limits();
    test_invalid_inputs();
    test_random_sequences();
    test_system_seams();
    puts("session_supervisor unit tests passed");
}
