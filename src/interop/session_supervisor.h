/**
 * @file session_supervisor.h
 * @brief Reconnect supervisor wrapping session_app: retry classification, capped backoff and cleanup.
 *
 * Each attempt is a fresh admission through session_app. Only outcomes that are definite
 * pre-admission refusals or a dropped guest are retried; everything else ends the supervisor.
 * The backoff runs 250 ms, 500 ms, 1 s, 2 s, then 4 s while retries continue, and restarts at
 * 250 ms after an attempt that reached Ready and stayed up for at least
 * SessionSupervisorStableMs. Time, waiting, the runner and cleanup are injected seams.
 */
#ifndef WaddleInteropSessionSupervisorH
#define WaddleInteropSessionSupervisorH

#include <stddef.h>
#include <stdint.h>

#include "session_app.h"
#include "session_control_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief First retry delay, in milliseconds. */
enum { SessionSupervisorInitialBackoffMs = 250 };
/** @brief Maximum retry delay, in milliseconds. */
enum { SessionSupervisorMaxBackoffMs = 4000 };
/** @brief Ready duration, in milliseconds, after which a session resets the backoff. */
enum { SessionSupervisorStableMs = 10000 };
/** @brief Bounded cleanup retries between attempts before the supervisor gives up. */
enum { SessionSupervisorCleanupRetries = 3 };

/** @brief Retry class of one attempt outcome. */
typedef enum session_supervisor_class_t {
    SessionSupervisorTerminal = 0, /**< Stop supervising and return the outcome. */
    SessionSupervisorRetry = 1 /**< Wait the scheduled backoff, then re-admit from scratch. */
} session_supervisor_class_t;

/** @brief Injectable waiting, runner and cleanup seams. All members are required.
 * The stability window reads the ready and end times the runtime reports in its detail.
 */
typedef struct session_supervisor_seams_t {
    void *context; /**< Passed unchanged to every seam. */
    /** @brief Wait up to @p ms unless @p stop_fd becomes readable.
     *  @return 0 when the full wait elapsed, 1 when stop_fd is readable, negative on OS failure. */
    int (*wait_ms)(void *context, uint32_t ms, int stop_fd);
    /** @brief Return 1 when @p stop_fd is readable now, without reading it; otherwise 0. */
    int (*stop_requested)(void *context, int stop_fd);
    /** @brief Run one attempt; the owners are zero on entry. */
    interop_host_control_result_t (*run)(void *context, const session_app_config_t *app,
        interop_host_control_owners_t *owners, interop_host_control_detail_t *detail);
    /** @brief Retry cleanup of owners left by a CleanupRequired attempt. */
    interop_host_control_result_t (*retry_cleanup)(void *context, interop_host_control_owners_t *owners);
} session_supervisor_seams_t;

/** @brief Supervisor inputs; the application config is borrowed until the call returns. */
typedef struct session_supervisor_config_t {
    session_app_config_t app; /**< Per-attempt application config; control.stop_fd is the stop descriptor. */
    session_supervisor_seams_t seams; /**< Injected seams. */
    uint32_t max_retries; /**< Retry limit; 0 means unlimited until a terminal outcome or stop. */
} session_supervisor_config_t;

/** @brief Counters reported for one supervised run. */
typedef struct session_supervisor_report_t {
    uint32_t attempts; /**< Admissions started. */
    uint32_t retries; /**< Backoff waits completed or started. */
    uint32_t ready_sessions; /**< Attempts that reached Ready. */
    uint32_t cleanup_retries; /**< retry_cleanup calls made between attempts. */
    uint64_t backoff_total_ms; /**< Sum of scheduled backoff delays. */
    interop_host_control_result_t final_outcome; /**< Result returned to the caller. */
} session_supervisor_report_t;

/** @brief Classify one attempt outcome for the retry decision.
 * @param[in] outcome Outcome returned by the runner.
 * @param[in] detail Diagnostic detail of the same attempt; NULL is treated as all zero.
 * @return Retry for definite pre-admission refusals and for a dropped guest; Terminal otherwise.
 * @thread_safety Pure function.
 */
session_supervisor_class_t session_supervisor_classify(
    interop_host_control_result_t outcome,
    const interop_host_control_detail_t *detail);

/** @brief Run supervised sessions until a terminal outcome, a stop request or the retry limit.
 * @param[in] config Aligned readable configuration until return.
 * @param[out] out_owners Aligned owner pair, zero on entry; holds any owner whose cleanup still
 *             fails when the supervisor returns CleanupRequired.
 * @param[out] out_report Optional counters; NULL allowed.
 * @return The final outcome. InvalidArgument for a NULL config, owners or missing seam. Cancelled
 *         when stop is requested before, during or between attempts.
 * @thread_safety Not thread-safe; the runner and cleanup run on the calling thread.
 */
interop_host_control_result_t session_supervisor_run(
    const session_supervisor_config_t *config,
    interop_host_control_owners_t *out_owners,
    session_supervisor_report_t *out_report);

/** @brief Seams backed by the monotonic clock, poll(2) on the stop descriptor and the real runtime. */
session_supervisor_seams_t session_supervisor_system_seams(void);

#ifdef __cplusplus
}
#endif
#endif
