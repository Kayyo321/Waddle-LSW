/**
 * @file session_control_host.h
 * @brief Synchronous control-only host runtime joining admission, mapping, and peer pump.
 */
#ifndef WaddleInteropSessionControlHostH
#define WaddleInteropSessionControlHostH

#include "interop_mapping_linux.h"
#include "session_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Persistent cleanup ownership returned when one cleanup step fails. */
typedef struct interop_host_control_owners_t {
    interop_linux_session_owner_t *session_owner; /**< Session, socket, peer, and lease owner. */
    interop_mapping_linux_t *mapping_owner; /**< Independently retryable local mapping owner. */
} interop_host_control_owners_t;

/** @brief Outcome of the synchronous host control-only event pump. */
typedef enum interop_host_control_result_t {
    HostControlSuccess = 0, /**< Admission, mapping validation, and bounded run completed. */
    HostControlInvalidArgument = 1, /**< Invalid or inconsistent borrowed input. */
    HostControlAdmissionFailed = 2, /**< Existing bounded admission entry rejected the session. */
    HostControlMappingFailed = 3, /**< Backing mapping or Hello-selected header validation failed. */
    HostControlPeerFailed = 4, /**< Socket owner or control protocol reached a terminal failure. */
    HostControlUnexpectedFeature = 5, /**< Capability-zero session received a feature frame. */
    HostControlDaemonLost = 6, /**< Borrowed local daemon control stream closed or sent unsolicited data. */
    HostControlCancelled = 7, /**< signalfd/cancel descriptor requested local stop. */
    HostControlTimedOut = 8, /**< Post-admission diagnostic run deadline expired. */
    HostControlOsError = 9, /**< Local operating-system operation failed. */
    HostControlCleanupRequired = 10 /**< One or both authentic owners remain in out_owners. */
} interop_host_control_result_t;

/** @brief Inputs for one control-only diagnostic session; all descriptors remain borrowed. */
typedef struct interop_host_control_config_t {
    const interop_host_session_config_t *admission; /**< Existing bounded admission inputs. */
    int mapping_fd; /**< Existing open O_RDWR backing object; never closed by this function. */
    interop_mapping_linux_backing_t backing_kind; /**< Explicit file/KVMFR kind; never inferred. */
    int stop_fd; /**< Borrowed nonblocking CLOEXEC signalfd for SIGINT/SIGTERM records; never closed. */
    uint32_t run_for_ms; /**< Post-admission bounded diagnostic interval, 1..120000. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_host_control_config_t;

/** @brief Run one host control-only admission and event-pump lifecycle.
 * @param[in] config Aligned readable config and nested admission inputs until return.
 * @param[out] out_owners Aligned writable zero-valued owner pair; holds any authentic
 *        session/mapping owner whose cleanup did not complete.
 * @return HostControlSuccess or a bounded operational outcome. CleanupRequired overrides
 *         the operational outcome only when local cleanup cannot relinquish an owner.
 * @note The creating pthread is the sole mapping and peer user. Capabilities are fixed to
 *       zero. The mapping is opened before peer polling, but its header is inspected only
 *       after the peer emits ValidateMapping with its actual Hello epoch. Host heartbeat
 *       starts only after Ready. Local mapping quiescence is asserted only after this
 *       synchronous pump has stopped using its view. Stop readiness is accepted only after
 *       one complete signalfd_siginfo for SIGINT/SIGTERM; EAGAIN is spurious, while short,
 *       invalid, or failed reads are operating-system errors. No guest drain, compositor detach,
 *       remote epoch-reuse, or backing-file reuse proof is produced.
 * @thread_safety Not thread-safe; all owner calls and descriptor use are serialized here.
 */
interop_host_control_result_t interop_host_control_run(
    const interop_host_control_config_t *config,
    interop_host_control_owners_t *out_owners);

/** @brief Retry cleanup of owners previously returned by run.
 * @param[in,out] owners Authentic owners from run; NULL members are idempotent.
 * @return HostControlSuccess when all local ownership is relinquished, otherwise
 *         HostControlCleanupRequired with every remaining owner still published.
 * @note Caller must invoke only after the original synchronous event pump returned and
 *       every local user of the mapping/peer has stopped. This function does not establish
 *       or infer remote quiescence.
 * @thread_safety Must run on the creating pthread and be serialized with all owner access.
 */
interop_host_control_result_t interop_host_control_retry_cleanup(
    interop_host_control_owners_t *owners);

#ifdef __cplusplus
}
#endif
#endif
