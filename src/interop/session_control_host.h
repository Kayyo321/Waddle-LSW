/**
 * @file session_control_host.h
 * @brief Synchronous control-only host runtime joining admission, mapping, and peer pump.
 */
#ifndef WaddleInteropSessionControlHostH
#define WaddleInteropSessionControlHostH

#include "interop_mapping_linux.h"
#include "session_host.h"

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum extra descriptors a feature module may multiplex into the pump. */
#define InteropHostFeatureFdCapacity 8u

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
    HostControlCleanupRequired = 10, /**< One or both authentic owners remain in out_owners. */
    HostControlFeatureFailed = 11 /**< A feature ops callback failed, or poll_fds exceeded its capacity; drain began. */
} interop_host_control_result_t;

/** @brief Opaque sender passed to feature callbacks; valid only while the receiving callback runs. */
typedef struct interop_host_feature_sender_t interop_host_feature_sender_t;

/** @brief Result of one feature sender operation; zero is success. Positive values are a closed set. */
typedef enum interop_host_feature_result_t {
    HostFeatureSuccess = 0, /**< Frame admitted, identifier released, or identifier written. */
    HostFeatureInvalidArgument = 1, /**< NULL or invalid sender, NULL pointer with nonzero length, or bad output pointer. */
    HostFeatureNotReady = 2, /**< Peer is not Ready for admission (handshake or other non-Ready phase). */
    HostFeatureDraining = 3, /**< Closing has begun or the Ready deadline expired at this send; nothing queued. */
    HostFeatureInvalidFrame = 4, /**< Frame malformed, or identifier class violated. */
    HostFeatureUnsupportedFrame = 5, /**< Type not legal for this entry point or producer. */
    HostFeatureNotNegotiated = 6, /**< Facility absent from the negotiated set. */
    HostFeatureQueueFull = 7, /**< Data-class queue full; retry after queued bytes are transmitted. */
    HostFeatureRequestIdsFull = 8, /**< 4032 feature identifiers are live. */
    HostFeatureUnknownRequestId = 9, /**< Identifier is not a live feature reservation. */
    HostFeatureRequestRetired = 10, /**< Transport loss retired every identifier; benign. */
    HostFeatureOutOfMemory = 11, /**< Queue copy allocation failed. */
    HostFeatureClosed = 12, /**< Transport closed. */
    HostFeatureTransportFailed = 13 /**< Internal failure; the run ends with PeerFailed. */
} interop_host_feature_result_t;

/** @brief Feature callback return value requesting a graceful local stop.
 * The runtime begins drain and the run ends with HostControlCancelled. Other handlers are not
 * failed; every ops callback still receives on_closing exactly once. Other nonzero values fail the run.
 */
enum { InteropHostFeatureStopRequested = 1 };

/** @brief Feature module callbacks and capability offer; every callback except the pair
 *         poll_fds/on_fds and next_deadline_ms is optional, and NULL is never called.
 * @note The runtime invokes callbacks only on the creating pthread, from inside its pump,
 *       never re-entrantly, and never after drain has begun except on_closing.
 */
typedef struct interop_host_feature_ops_t {
    void *context; /**< Passed unchanged to every callback; may be NULL. */
    uint64_t local_capabilities; /**< Offered in Hello; within InteropCapabilityMask and equal to admission. */
    /** @brief Called once after Ready. Return 0 to continue, nonzero to fail and drain.
     *  @param[in] negotiated_capabilities Peer intersection, a subset of local_capabilities.
     *  @param[in] sender Valid only during this call. */
    int (*on_ready)(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender);
    /** @brief Called for each admitted inbound feature frame. Return nonzero to fail and drain.
     *  @param[in] frame Borrowed copied bytes, valid only during this call; length equals the frame size. */
    int (*on_frame)(void *context, uint32_t message_type, uint32_t request_id,
                    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender);
    /** @brief Supply up to capacity descriptors; the runtime never closes them.
     *  @param[out] fds Array of capacity entries; the runtime clears revents after the call.
     *  @param[in] capacity Always InteropHostFeatureFdCapacity.
     *  @param[out] count Entries written; more than capacity is a failure. */
    int (*poll_fds)(void *context, struct pollfd *fds, size_t capacity, size_t *count);
    /** @brief Called after a poll cycle in which at least one supplied descriptor reported events. */
    int (*on_fds)(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
                  interop_host_feature_sender_t *sender);
    /** @brief Called at least every 100 ms while Ready and not draining, before the socket poll. */
    int (*on_tick)(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender);
    /** @brief Absolute CLOCK_MONOTONIC millisecond deadline, or UINT64_MAX; caps the poll wait. */
    uint64_t (*next_deadline_ms)(void *context, uint64_t now_ms);
    /** @brief Called exactly once when drain begins or the run ends, before owner cleanup.
     *  @param[in] reason interop_host_control_result_t value recorded at this moment.
     *  @param[in] sender Valid only during this call; every admission returns HostFeatureDraining. */
    void (*on_closing)(void *context, uint32_t reason, interop_host_feature_sender_t *sender);
} interop_host_feature_ops_t;

/** @brief Inputs for one control-only diagnostic session; all descriptors remain borrowed. */
typedef struct interop_host_control_config_t {
    const interop_host_session_config_t *admission; /**< Existing bounded admission inputs. */
    int mapping_fd; /**< Existing open O_RDWR backing object; never closed by this function. */
    interop_mapping_linux_backing_t backing_kind; /**< Explicit file/KVMFR kind; never inferred. */
    int stop_fd; /**< Borrowed nonblocking CLOEXEC signalfd for SIGINT/SIGTERM records; never closed. */
    uint32_t run_for_ms; /**< Post-admission bounded interval: 1..120000, or 0 (only with feature_ops) for no bound. */
    uint32_t reserved_zero; /**< Must be zero. */
    const interop_host_feature_ops_t *feature_ops; /**< Optional feature module; NULL keeps capability-zero behaviour. */
} interop_host_control_config_t;

/** @brief Diagnostic detail of one control run, filled by interop_host_control_run_detailed.
 * @note Every field is zero on entry; detail is written only by the owning pump thread.
 */
typedef struct interop_host_control_detail_t {
    uint32_t admission_result; /**< interop_host_session_result_t when the outcome is AdmissionFailed; else 0. */
    uint32_t reached_ready; /**< 1 when Ready was committed during this run. */
    uint32_t guest_error; /**< 1 when the guest sent a protocol Error or an unadmitted feature frame. */
    uint32_t reserved_zero; /**< Must be zero. */
    uint64_t ready_at_ms; /**< Monotonic milliseconds when Ready was committed; zero when not reached. */
    uint64_t ended_at_ms; /**< Monotonic milliseconds when the pump stopped; zero when unavailable. */
} interop_host_control_detail_t;

/** @brief Run one host control-only lifecycle and report diagnostic detail.
 * @param[in] config As for interop_host_control_run.
 * @param[out] out_owners As for interop_host_control_run.
 * @param[out] detail Optional aligned output disjoint from config and owners; zeroed then filled; NULL allowed.
 * @return The same outcome interop_host_control_run returns for the same inputs.
 * @thread_safety Not thread-safe; the creating pthread is the only user.
 */
interop_host_control_result_t interop_host_control_run_detailed(
    const interop_host_control_config_t *config,
    interop_host_control_owners_t *out_owners,
    interop_host_control_detail_t *detail);

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

/** @brief Send one unsolicited or response feature frame from a feature callback.
 * @param[in,out] sender Valid sender from the callback that is currently running.
 * @param[in] frame Borrowed complete frame; NULL only with length zero (InvalidFrame).
 * @param[in] length Exact frame size.
 * @return HostFeatureSuccess, or the refusal reason; nothing is queued on any error.
 * @note Request types must use interop_host_feature_send_request. Draining is returned after
 *       closing has begun and when the Ready deadline expires; the run then drains.
 * @thread_safety Callback thread only; the runtime serializes all sender use.
 */
int interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length);

/** @brief Send one Request frame from a feature callback and receive its identifier.
 * @param[in,out] sender Valid sender from the running callback.
 * @param[in] frame Borrowed request frame whose header identifier is zero.
 * @param[in] length Exact frame size.
 * @param[out] out_request_id Non-NULL; written only when the result is HostFeatureSuccess.
 * @return HostFeatureSuccess, or the refusal reason; nothing is queued and no identifier is kept on error.
 * @thread_safety Callback thread only.
 */
int interop_host_feature_send_request(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length, uint32_t *out_request_id);

/** @brief Release a feature request identifier once its response arrived or the request was abandoned.
 * @param[in,out] sender Valid sender from the running callback; allowed while closing.
 * @param[in] request_id Identifier from interop_host_feature_send_request.
 * @return HostFeatureSuccess, UnknownRequestId for identifiers that are not live feature reservations,
 *         or RequestRetired after transport loss.
 * @thread_safety Callback thread only.
 */
int interop_host_feature_release_request(interop_host_feature_sender_t *sender, uint32_t request_id);

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
