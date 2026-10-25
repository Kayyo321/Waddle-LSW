#ifndef WaddleInteropSessionControlWindowsH
#define WaddleInteropSessionControlWindowsH
/**
 * @file session_control_windows.h
 * @brief One bounded Windows guest control session over native Viosock.
 *
 * The runtime joins the frozen Windows socket owner, Windows mapping owner,
 * layout-v3 initializer and peer ABI. It does not produce remote detach proof.
 */
#include <stddef.h>
#include <stdint.h>
#if defined(InteropGuestSessionTestApi)
#include "session_control_windows_api_double.h"
#else
#include <winsock2.h>
#include <windows.h>
#endif
#include "windows_socket_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque resource owner retained whenever cleanup is incomplete. */
typedef struct interop_guest_session_owner_t interop_guest_session_owner_t;

/** @brief Maximum absolute wait for one native host connection. */
#define InteropGuestSessionAcceptTimeoutMs 30000u
/** @brief Maximum absolute run interval after one valid peer is accepted. */
#define InteropGuestSessionRunTimeoutMs 120000u
/** @brief Guest heartbeat period after Ready. */
#define InteropGuestSessionHeartbeatPeriodMs 500u
/** @brief Frozen socket-owner graceful drain interval. */
#define InteropGuestSessionDrainTimeoutMs 5000u
/** @brief Maximum wait slice before checking stop and absolute deadlines. */
#define InteropGuestSessionPollSliceMs 100u
/** @brief The actual Viosock family query IOCTL used by the guest listener. */
#define InteropGuestSessionViosockFamilyIoctl 0x0801300cu
/** @brief Windows host CID admitted by the guest listener policy. */
#define InteropGuestSessionHostCid 2u

/** @brief Session-run result; cleanup diagnostics remain separately queryable. */
typedef enum interop_guest_session_result_t {
    InteropGuestSessionSuccess = 0, /**< Bounded control-only attempt finished and cleanup succeeded. */
    InteropGuestSessionInvalidArgument = 1, /**< Config/output pointer, range, alignment, or reserved field invalid. */
    InteropGuestSessionFreshAuthorizationRequired = 2, /**< Trusted fresh-exclusive precondition absent; no native work began. */
    InteropGuestSessionOutOfMemory = 3, /**< Controller allocation failed before native ownership. */
    InteropGuestSessionOsError = 4, /**< Win32, WinSock, clock, or random-source operation failed. */
    InteropGuestSessionTimedOut = 5, /**< Absolute accept, handshake, or run deadline expired. */
    InteropGuestSessionStopped = 6, /**< Caller-owned stop event was signaled. */
    InteropGuestSessionRejectedPeer = 7, /**< Accepted peer address did not satisfy host CID policy. */
    InteropGuestSessionMappingFailed = 8, /**< Mapping open, metadata init, validation, or heartbeat failed. */
    InteropGuestSessionPeerFailed = 9, /**< Frozen peer/socket owner reported a terminal protocol or I/O failure. */
    InteropGuestSessionUnexpectedFeature = 10, /**< Capability-zero diagnostic session received a feature frame. */
    InteropGuestSessionCleanupRequired = 11, /**< Exact native owner remains published for caller retry. */
    InteropGuestSessionWrongThread = 12 /**< Operation called outside the creating Windows thread. */
} interop_guest_session_result_t;

/** @brief Copied local resource facts; contains no native handles or mapped bytes. */
typedef struct interop_guest_session_state_t {
    uint32_t winsock_started; /**< Exactly 0 or 1; one Winsock startup reference is retained. */
    uint32_t family_handle_owned; /**< Exactly 0 or 1; Viosock query HANDLE remains recorded. */
    uint32_t listener_owned; /**< Exactly 0 or 1; native listener SOCKET remains recorded. */
    uint32_t accepted_socket_owned; /**< Exactly 0 or 1; raw accepted SOCKET not yet transferred. */
    uint32_t socket_owner_owned; /**< Exactly 0 or 1; frozen socket owner remains recorded. */
    uint32_t mapping_owner_owned; /**< Exactly 0 or 1; frozen mapping owner remains recorded. */
    uint32_t mapping_view_published; /**< Sticky local history; requires separate host-detach assertion to release. */
    uint32_t reserved_zero; /**< Always zero. */
    uint32_t operational_result; /**< First non-cleanup interop_guest_session_result_t. */
    uint32_t last_win32_error; /**< First copied GetLastError or deterministic ERROR_INVALID_DATA diagnostic, or zero. */
    uint32_t last_wsa_error; /**< Most recently copied WSAGetLastError or direct WSAStartup return code, or zero. */
    uint32_t mapping_status; /**< Last copied interop_mapping_windows_status_t, or zero. */
    uint32_t cleanup_error_count; /**< Number of independently failed cleanup calls observed. */
    uint32_t last_ntstatus; /**< Exact BCryptGenRandom NTSTATUS bits, or zero when no RNG call failed. */
} interop_guest_session_state_t;

/** @brief Borrowed, immutable inputs to one single-session run. */
typedef struct interop_guest_session_config_t {
    uint32_t device_index; /**< Explicit frozen mapping device selection; no automatic fallback. */
    uint32_t fresh_exclusive_authorized; /**< Exactly 1 only after trusted caller independently established a fresh exclusive mapping. */
    uint64_t connection_token; /**< Nonzero stale-operation token; not authentication. */
    HANDLE stop_event; /**< Optional borrowed caller event; NULL disables stop. Never signaled/reset/closed here. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_guest_session_config_t;

/** @brief Caller-owned WinSock lifetime transferred with an unresolved socket. */
typedef struct interop_guest_session_quarantine_t {
    interop_windows_socket_quarantine_t sockets[3]; /**< Exact unresolved listener, accepted, and session SOCKET records. */
    uint32_t socket_count; /**< Number of populated records, 1..3. */
    uint32_t winsock_reference_owned; /**< Exactly 1 until matching release succeeds. */
    uint32_t reserved_zero; /**< Always zero. */
    uint32_t last_wsa_error; /**< Copied WSACleanup diagnostic from the last failed release attempt. */
} interop_guest_session_quarantine_t;

/**
 * @brief Run one native Viosock host admission and bounded control-only session.
 * @param[in] config Naturally aligned, readable borrowed config for this call.
 * @param[out] owner_out Naturally aligned writable NULL-valued owner slot,
 *                        disjoint from config and all nested input bytes.
 * @return Success or an operational result when every local resource was
 *         released; CleanupRequired when an authentic native owner remains.
 * @note The caller must set fresh_exclusive_authorized only from an independent
 *       trusted authority. The runner uses Viosock family query, local guest
 *       port 5002, remote CID 2, zero capabilities, one accepted peer, and a
 *       fresh cryptographic epoch. All operations run on the creating thread.
 *       EOF, drain, timeout, stop, heartbeat and process exit never prove host
 *       detach or authorize epoch reuse.
 */
interop_guest_session_result_t interop_guest_session_run(
    const interop_guest_session_config_t *config,
    interop_guest_session_owner_t **owner_out);

/**
 * @brief Retry cleanup of a retained session owner after run returned.
 * @param[in,out] owner_slot Aligned writable slot containing an authentic owner.
 * @param[in] host_detached_assertion Exactly 0 or 1. Required as 1 only when an
 *        independent caller has established host detach; protocol/EOF/timeout
 *        cannot provide this assertion.
 * @return Success and NULLs the slot only after every owned resource is gone;
 *         CleanupRequired retains all unresolved exact owners; InvalidArgument
 *         or WrongThread preserves the owner slot and resources.
 * @note Caller must have stopped all local mapping and socket users. One close
 *       retry is attempted for a frozen socket owner already ClosePending.
 */
interop_guest_session_result_t interop_guest_session_retry_cleanup(
    interop_guest_session_owner_t **owner_slot,
    uint32_t host_detached_assertion);

/**
 * @brief Transfer all unresolved sockets and WinSock lifetime into quarantine.
 * @param[in,out] owner_slot Authentic retained owner; all resources except the
 *        pending or indeterminate sockets must be the only remaining native
 *        resources; published mapping ownership must already be released.
 * @param[out] quarantine_out Writable, aligned, disjoint, all-zero bundle.
 * @return Success transfers exact socket identity and its Winsock startup
 *         reference into the caller bundle and frees/nulls the session owner;
 *         failure preserves the owner and output.
 * @note Requires the creating thread. It never closes or retries an
 *       indeterminate SOCKET. Caller retains the bundle and Winsock reference
 *       until every exact socket is separately resolved, then calls release.
 *       This is not mapping detach proof, host drain proof, or epoch-reuse authority.
 */
interop_guest_session_result_t interop_guest_session_quarantine_socket(
    interop_guest_session_owner_t **owner_slot,
    interop_guest_session_quarantine_t *quarantine_out);

/**
 * @brief Release a quarantined socket's Winsock startup reference after external resolution.
 * @param[in,out] quarantine Caller-owned authentic bundle, owner thread only.
 * @param[in] socket_resolution_assertion Exactly 1 only after native socket
 *        resolution is independently complete; 0 is rejected without mutation.
 * @return Success clears the bundle after WSACleanup; OsError preserves it.
 * @note This does not close or retry the SOCKET. Caller must first resolve its
 *       exact socket through a separately authorized native path.
 */
interop_guest_session_result_t interop_guest_session_release_quarantine(
    interop_guest_session_quarantine_t *quarantine,
    uint32_t socket_resolution_assertion);

/**
 * @brief Copy resource-presence and error diagnostics from an authentic owner.
 * @param[in] owner Live retained owner, borrowed for this serialized call.
 * @param[out] state_out Aligned writable output disjoint from owner storage.
 * @return Success or InvalidArgument with output unchanged.
 * @note No handle, view, epoch, secret, or mapping bytes escape. Creating
 *       Windows thread only; all accesses are serialized with cleanup.
 */
interop_guest_session_result_t interop_guest_session_get_state(
    const interop_guest_session_owner_t *owner,
    interop_guest_session_state_t *state_out);

#ifdef __cplusplus
}
#endif
#endif
