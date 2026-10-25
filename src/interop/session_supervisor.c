/**
 * @file session_supervisor.c
 * @brief Reconnect supervisor: attempt classification, capped backoff, stop and cleanup handling.
 */
#include "session_supervisor.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/** @brief Monotonic clock in milliseconds for the system seams; UINT64_MAX on failure. */
static uint64_t supervisor_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

session_supervisor_class_t session_supervisor_classify(
    interop_host_control_result_t outcome,
    const interop_host_control_detail_t *detail)
{
    interop_host_control_detail_t none;
    memset(&none, 0, sizeof(none));
    if (detail == NULL) detail = &none;
    switch (outcome) {
    case HostControlAdmissionFailed:
        switch (detail->admission_result) {
        case HostSessionNotRunning:
        case HostSessionGuestUnavailable:
        case HostSessionConnectError:
        case HostSessionDaemonError:
        case HostSessionTimedOut:
            return SessionSupervisorRetry;
        default:
            return SessionSupervisorTerminal;
        }
    case HostControlPeerFailed:
        return detail->guest_error != 0 ? SessionSupervisorTerminal : SessionSupervisorRetry;
    case HostControlDaemonLost:
    case HostControlTimedOut:
        return SessionSupervisorRetry;
    default:
        return SessionSupervisorTerminal;
    }
}

/** @brief poll(2) on the stop descriptor: 1 when readable or failed, 0 on timeout, -1 when interrupted. */
static int supervisor_poll_stop(int stop_fd, int timeout_ms)
{
    struct pollfd descriptor;
    int ready;

    descriptor.fd = stop_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0) return errno == EINTR ? -1 : 1;
    return ready > 0 ? 1 : 0;
}

static int system_wait_ms(void *context, uint32_t ms, int stop_fd)
{
    uint64_t deadline;
    (void)context;
    deadline = supervisor_monotonic_ms() + ms;
    for (;;) {
        uint64_t now = supervisor_monotonic_ms();
        uint64_t remaining;
        int ready;
        if (now >= deadline) return 0;
        remaining = deadline - now;
        ready = supervisor_poll_stop(stop_fd, (int)(remaining < 1000 ? remaining : 1000));
        if (ready == 1) return 1;
    }
}

static int system_stop_requested(void *context, int stop_fd)
{
    (void)context;
    return supervisor_poll_stop(stop_fd, 0) == 1 ? 1 : 0;
}

static interop_host_control_result_t system_run(void *context, const session_app_config_t *app,
    interop_host_control_owners_t *owners, interop_host_control_detail_t *detail)
{
    (void)context;
    return session_app_run_detailed(app, owners, detail);
}

static interop_host_control_result_t system_retry_cleanup(void *context,
    interop_host_control_owners_t *owners)
{
    (void)context;
    return interop_host_control_retry_cleanup(owners);
}

session_supervisor_seams_t session_supervisor_system_seams(void)
{
    session_supervisor_seams_t seams;
    memset(&seams, 0, sizeof(seams));
    seams.wait_ms = system_wait_ms;
    seams.stop_requested = system_stop_requested;
    seams.run = system_run;
    seams.retry_cleanup = system_retry_cleanup;
    return seams;
}

/** @brief Delay after the current one: doubles up to the cap. */
static uint32_t supervisor_next_delay(uint32_t delay)
{
    uint32_t next = delay * 2;
    return next > SessionSupervisorMaxBackoffMs ? SessionSupervisorMaxBackoffMs : next;
}

/** @brief True when the attempt reached Ready and stayed up for the whole stability window. */
static bool supervisor_stable(const interop_host_control_detail_t *detail)
{
    return detail->reached_ready != 0 && detail->ended_at_ms >= detail->ready_at_ms &&
        detail->ended_at_ms - detail->ready_at_ms >= SessionSupervisorStableMs;
}

interop_host_control_result_t session_supervisor_run(
    const session_supervisor_config_t *config,
    interop_host_control_owners_t *out_owners,
    session_supervisor_report_t *out_report)
{
    session_supervisor_report_t report;
    interop_host_control_result_t final_outcome = HostControlInvalidArgument;
    uint32_t delay = SessionSupervisorInitialBackoffMs;
    int stop_fd;

    memset(&report, 0, sizeof(report));
    if (out_report != NULL) memset(out_report, 0, sizeof(*out_report));
    if (config == NULL || out_owners == NULL || config->seams.run == NULL ||
        config->seams.retry_cleanup == NULL || config->seams.wait_ms == NULL ||
        config->seams.stop_requested == NULL || config->app.control.stop_fd < 0 ||
        out_owners->session_owner != NULL || out_owners->mapping_owner != NULL)
        goto done;
    stop_fd = config->app.control.stop_fd;
    for (;;) {
        interop_host_control_detail_t detail;
        interop_host_control_result_t outcome;
        int waited;

        if (config->seams.stop_requested(config->seams.context, stop_fd)) {
            final_outcome = HostControlCancelled;
            break;
        }
        memset(&detail, 0, sizeof(detail));
        ++report.attempts;
        outcome = config->seams.run(config->seams.context, &config->app, out_owners, &detail);
        if (detail.reached_ready != 0) ++report.ready_sessions;
        if (outcome == HostControlCleanupRequired) {
            interop_host_control_result_t cleaned = HostControlCleanupRequired;
            uint32_t tries;

            for (tries = 0; tries < SessionSupervisorCleanupRetries &&
                    cleaned == HostControlCleanupRequired; ++tries) {
                ++report.cleanup_retries;
                cleaned = config->seams.retry_cleanup(config->seams.context, out_owners);
            }
            /* A resolved cleanup leaves the attempt's operational result unknown: terminal. */
            final_outcome = cleaned == HostControlSuccess ? HostControlOsError : HostControlCleanupRequired;
            break;
        }
        if (session_supervisor_classify(outcome, &detail) == SessionSupervisorTerminal) {
            final_outcome = outcome;
            break;
        }
        if (config->max_retries != 0 && report.retries >= config->max_retries) {
            final_outcome = outcome;
            break;
        }
        if (supervisor_stable(&detail)) delay = SessionSupervisorInitialBackoffMs;
        if (config->seams.stop_requested(config->seams.context, stop_fd)) {
            final_outcome = HostControlCancelled;
            break;
        }
        ++report.retries;
        report.backoff_total_ms += delay;
        waited = config->seams.wait_ms(config->seams.context, delay, stop_fd);
        if (waited == 1) {
            final_outcome = HostControlCancelled;
            break;
        }
        if (waited < 0) {
            final_outcome = HostControlOsError;
            break;
        }
        delay = supervisor_next_delay(delay);
    }

done:
    report.final_outcome = final_outcome;
    if (out_report != NULL) *out_report = report;
    return final_outcome;
}
