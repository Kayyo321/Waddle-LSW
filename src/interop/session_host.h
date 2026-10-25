/**
 * @file session_host.h
 * @brief Bounded Linux host session admission joining daemon readiness and interop transport.
 */
#ifndef WaddleInteropSessionHostH
#define WaddleInteropSessionHostH

#include <stddef.h>
#include <stdint.h>
#include "linux_session_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Local result of one host session admission attempt. */
typedef enum interop_host_session_result_t {
    HostSessionSuccess = 0, /**< One connected host peer and both leases are owned. */
    HostSessionInvalidArgument = 1, /**< Invalid, overlapping, unaligned, or malformed input. */
    HostSessionOutOfMemory = 2, /**< Existing lease owner allocation failed. */
    HostSessionBusy = 3, /**< Session singleton lease is already held. */
    HostSessionAvBusy = 4, /**< AV singleton lease is already held. */
    HostSessionCancelled = 5, /**< Cancellation was observed before or during admission. */
    HostSessionTimedOut = 6, /**< The one absolute deadline expired. */
    HostSessionDaemonError = 7, /**< Local daemon control I/O or protocol exchange failed. */
    HostSessionNotRunning = 8, /**< Daemon reported a non-Running subsystem. */
    HostSessionGuestUnavailable = 9, /**< GuestReady returned a valid nonzero status. */
    HostSessionConnectError = 10, /**< One interop VSOCK attempt failed. */
    HostSessionAttemptConsumed = 11, /**< Earlier ambiguous VSOCK attempt forbids retry. */
    HostSessionPeerCreateError = 12, /**< Connected stream could not become a socket owner. */
    HostSessionOsError = 13, /**< Local operating-system operation failed. */
    HostSessionClockError = 14, /**< Monotonic clock failed or returned invalid values. */
    HostSessionCloseDiagnostic = 15, /**< Admission cleanup completed with a close diagnostic. */
    HostSessionCleanupRequired = 16 /**< Owner published in out_owner; retry peer/lease cleanup. */
} interop_host_session_result_t;

/** @brief Immutable borrowed inputs for one host admission call.
 * @note `device->vsock_port` is the guest-exec readiness port. Interop transport
 *       uses the existing reserved port 5002. The creating pthread owns the
 *       returned session owner. No pointer or descriptor is retained by this API.
 */
typedef struct interop_host_session_config_t {
    const device_info_t *device; /**< Selected validated device profile; borrowed for this call. */
    int daemon_fd; /**< Connected per-device AF_UNIX SOCK_STREAM; caller retains ownership. */
    const uint8_t *agent_version; /**< SemVer2 bytes copied by the existing session owner. */
    size_t agent_version_bytes; /**< Version length, 5..32 bytes. */
    uint64_t connection_token; /**< Nonzero stale-callback token, not authentication. */
    uint64_t local_capabilities; /**< Implemented subset of InteropCapabilityMask. */
    uint64_t deadline_ms; /**< Absolute CLOCK_MONOTONIC deadline in milliseconds. */
    int cancel_fd; /**< -1 disables cancellation; otherwise stable pollable borrowed fd. */
    uint32_t guest_ready_timeout_ms; /**< GuestReady probe bound, 1..120000 ms. */
    uint32_t drain_timeout_ms; /**< Socket drain bound, 1..60000 ms. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_host_session_config_t;

/** @brief Admit one host peer after local daemon and guest-exec readiness checks.
 * @param[in] config Aligned borrowed config and disjoint nested ranges, readable until return.
 *         `daemon_fd` must already be connected to the selected device's daemon endpoint.
 * @param[out] out_owner Aligned writable NULL-valued slot; published on success, or with an
 *         authentic cleanup-required owner if destruction could not complete after admission.
 * @param[out] out_control_channel_invalidated Aligned writable u32; set to zero after argument
 *         preflight, then to one if an interrupted/malformed transaction leaves daemon_fd unsafe.
 * @return HostSessionSuccess or a precise bounded admission failure. errno is preserved for OS
 *         failures and is restored after best-effort lease cleanup.
 * @note Checks SO_PEERCRED uid against geteuid before daemon I/O. A single monotonic deadline
 *       covers status, GuestReady, and VSOCK connect. Cancellation is polled with each local
 *       exchange and passed through to the existing VSOCK owner. Malformed/partial exchanges
 *       invalidate the borrowed control channel; this function never closes it. Exactly one
 *       interop VSOCK connect call is made. No mapping or Wayland ownership/proof is created.
 * @thread_safety Calls and daemon_fd use are serialized; all owner operations stay on caller pthread.
 */
interop_host_session_result_t interop_host_session_admit(
    const interop_host_session_config_t *config,
    interop_linux_session_owner_t **out_owner,
    uint32_t *out_control_channel_invalidated);

#ifdef __cplusplus
}
#endif
#endif
