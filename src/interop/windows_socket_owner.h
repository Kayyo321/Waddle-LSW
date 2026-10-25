#ifndef WaddleInteropWindowsSocketOwnerH
#define WaddleInteropWindowsSocketOwnerH
/**
 * @file windows_socket_owner.h
 * @brief Pull-driven Windows socket owner for one serialized interop peer.
 *
 * This transport-agnostic byte owner accepts an already-connected WinSock
 * stream socket. It does not implement a VSOCK listener, own an IVSHMEM map,
 * or establish host/guest resource-release proof.
 */
#include <stddef.h>
#include <stdint.h>
#if defined(InteropWindowsSocketTestApi)
#include "windows_socket_owner_api_double.h"
#else
#include <winsock2.h>
#include <windows.h>
#endif
#include <waddle/interop_peer.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque, noncopyable owner returned by create. */
typedef struct interop_windows_socket_t interop_windows_socket_t;

/** @brief Fixed copied RX staging bound; peer ABI retains partial frames. */
#define InteropWindowsSocketRxCarryBytes 8192u
/** @brief Maximum accepted TX bytes per drive call. */
#define InteropWindowsSocketMaxTxBudgetBytes 65536u
/** @brief Maximum accepted RX bytes per drive call. */
#define InteropWindowsSocketMaxRxBudgetBytes 8192u
/** @brief Maximum combined recv/send calls per drive call. */
#define InteropWindowsSocketMaxSyscallBudget 64u
/** @brief Maximum combined peer/helper operations per drive call. */
#define InteropWindowsSocketMaxInternalStepsPerDrive 64u
/** @brief Upper bound for a caller-configured graceful drain interval. */
#define InteropWindowsSocketMaxDrainTimeoutMs 60000u
/** @brief Required maximum interval between timer-advancing drives. */
#define InteropWindowsSocketMaxDriveIntervalMs 100u
/** @brief Required caller-owned event-frame capacity. */
#define InteropWindowsSocketFrameCapacity 65536u

/** @brief Local API result, separate from peer protocol errors. */
typedef enum interop_windows_socket_result_t {
    InteropWindowsSocketSuccess = 0, /**< Operation completed; inspect outputs. */
    InteropWindowsSocketInvalidArgument = 1, /**< Pointer, range, alias, or value invalid. */
    InteropWindowsSocketOutOfMemory = 2, /**< No socket ownership transferred. */
    InteropWindowsSocketWrongToken = 3, /**< Stale connection token; no mutation. */
    InteropWindowsSocketWrongThread = 4, /**< Caller is not the captured owner thread. */
    InteropWindowsSocketTimeWentBackwards = 5, /**< Retry with nondecreasing monotonic time. */
    InteropWindowsSocketWrongPhase = 6, /**< Operation is invalid in the current state. */
    InteropWindowsSocketStalePoll = 7, /**< Poll cookie is not the current snapshot. */
    InteropWindowsSocketPollCookieExhausted = 8, /**< Poll cookie counter reached UINT64_MAX. */
    InteropWindowsSocketClosed = 9, /**< Operation requires a usable transport. */
    InteropWindowsSocketPeerFailure = 10, /**< Unexpected peer invariant; transport is terminal. */
    InteropWindowsSocketClosePending = 11, /**< WSAEWOULDBLOCK proves SOCKET remains valid. */
    InteropWindowsSocketCloseIndeterminate = 12, /**< Close failed without a validity guarantee. */
    InteropWindowsSocketMappingFailed = 13, /**< Guest mapping failure; no Hello was sent. */
    InteropWindowsSocketOsError = 14 /**< WinSock/Win32 call failed; error is copied where available. */
} interop_windows_socket_result_t;

/** @brief Owner state; close-pending states never claim transport loss. */
typedef enum interop_windows_socket_state_t {
    InteropWindowsSocketActive = 1, /**< Normal handshake or ready processing. */
    InteropWindowsSocketMappingWait = 2, /**< Explicit guest initialization or host validation. */
    InteropWindowsSocketDraining = 3, /**< Admission ended; finite TX flush only. */
    InteropWindowsSocketTransportClosed = 4, /**< Native close succeeded and peer was notified. */
    InteropWindowsSocketClosePendingState = 5, /**< Explicit close retry may be attempted. */
    InteropWindowsSocketCloseIndeterminateState = 6 /**< Only quarantine handoff is permitted. */
} interop_windows_socket_state_t;

/** @brief Operational outcome copied by drive. */
typedef enum interop_windows_socket_work_status_t {
    InteropWindowsSocketWorkIdle = 0, /**< No event, I/O, or terminal condition. */
    InteropWindowsSocketWorkProgress = 1, /**< Bytes moved or peer clock advanced. */
    InteropWindowsSocketWorkTransportClosed = 2, /**< EOF/error/abort/drain closed transport. */
    InteropWindowsSocketWorkPeerFailure = 3, /**< Peer invariant failed; inspect state. */
    InteropWindowsSocketWorkClosePending = 4, /**< Native close failed but SOCKET validity is documented. */
    InteropWindowsSocketWorkCloseIndeterminate = 5 /**< Native close failed without a validity guarantee. */
} interop_windows_socket_work_status_t;

/** @brief First preserved reason for transport closure or close quarantine. */
typedef enum interop_windows_socket_terminal_reason_t {
    InteropWindowsSocketTerminalNone = 0, /**< Native transport has not closed. */
    InteropWindowsSocketTerminalEof = 1, /**< recv returned zero. */
    InteropWindowsSocketTerminalSocketError = 2, /**< Fatal WinSock operation. */
    InteropWindowsSocketTerminalPollInvalid = 3, /**< Poll reported invalid socket. */
    InteropWindowsSocketTerminalPeerAbort = 4, /**< Peer requested immediate abort. */
    InteropWindowsSocketTerminalDrainDeadline = 5, /**< Finite drain deadline expired. */
    InteropWindowsSocketTerminalLocalAbort = 6, /**< Caller requested immediate abort. */
    InteropWindowsSocketTerminalPeerFailure = 7, /**< Unexpected peer ABI invariant. */
    InteropWindowsSocketTerminalDestroyed = 8, /**< Explicit destroy initiated close. */
    InteropWindowsSocketTerminalDrainComplete = 9 /**< Permitted TX queue emptied while draining. */
} interop_windows_socket_terminal_reason_t;

/** @brief Borrowed peer config and finite drain policy; not retained. */
typedef struct interop_windows_socket_config_t {
    interop_peer_config_t peer_config; /**< Borrowed during create; peer copies version bytes and retains no caller pointer. */
    uint32_t drain_timeout_ms; /**< 1..InteropWindowsSocketMaxDrainTimeoutMs. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_windows_socket_config_t;

/** @brief Fresh poll interests and timer guidance, with a single-use cookie. */
typedef struct interop_windows_socket_poll_t {
    uintptr_t socket_value; /**< Exact SOCKET value represented as uintptr_t; 0 when closed. */
    uint32_t events; /**< WSAPoll input interests: POLLRDNORM/POLLWRNORM only. */
    uint32_t timeout_ms; /**< Recommended wait, always 0..100 ms. */
    uint32_t immediate_work; /**< Exactly 0 or 1. */
    uint32_t reserved_zero; /**< Always zero. */
    uint64_t poll_cookie; /**< Nonzero single-use snapshot generation. */
} interop_windows_socket_poll_t;

/** @brief One drive result with copied peer event and terminal snapshot. */
typedef struct interop_windows_socket_work_t {
    uint32_t state; /**< interop_windows_socket_state_t. */
    uint32_t status; /**< interop_windows_socket_work_status_t. */
    size_t rx_bytes; /**< Bytes accepted by recv in this call. */
    size_t tx_bytes; /**< Bytes accepted by send in this call. */
    uint32_t syscalls; /**< recv/send attempts charged to the limit. */
    uint32_t internal_steps; /**< Peer/helper operations charged to the limit. */
    uint32_t terminal_reason; /**< interop_windows_socket_terminal_reason_t. */
    uint32_t wsa_error; /**< First preserved WSA error, otherwise zero. */
    uint32_t close_wsa_error; /**< Most recent closesocket error, independent of terminal error. */
    interop_peer_event_t event; /**< One newly consumed copied peer event. */
    interop_peer_event_t diagnostic_event; /**< Stable terminal event; never repeats a feature frame. */
} interop_windows_socket_work_t;

/** @brief Caller-owned evidence-free handoff for a socket whose close is unresolved. */
typedef struct interop_windows_socket_quarantine_t {
    uintptr_t socket_value; /**< Exact numeric SOCKET value; no authority to close or reuse it. */
    uint32_t wsa_error; /**< Copied WSAGetLastError result from failed closesocket. */
    uint32_t close_classification; /**< ClosePendingState or CloseIndeterminateState. */
    uint32_t owner_thread_id; /**< Captured GetCurrentThreadId token, diagnostic only. */
    uint32_t reserved_zero; /**< Always zero. */
    uint64_t connection_token; /**< Copied peer token, not authentication or epoch proof. */
} interop_windows_socket_quarantine_t;

/**
 * @brief Validate and take ownership of one connected WinSock stream socket.
 * @param[in] connected_socket Caller-owned connected SOCK_STREAM SOCKET.
 * @param[in] config Aligned readable config; copied by peer creation.
 * @param[out] out_owner Aligned writable NULL-valued owner slot, disjoint from inputs.
 * @return Success transfers socket ownership; failure leaves the caller owning it.
 * @note Caller keeps successful WSAStartup active and guarantees exclusive use,
 * no overlapped I/O, no pending operation/callback, no WSAAsyncSelect/WSAEventSelect,
 * and an already-nonblocking socket. This function verifies noninheritance,
 * SOCK_STREAM, connected peer, and SO_LINGER disabled. It calls FIONBIO(1) last;
 * WinSock has no query for the previous blocking mode, so no rollback is promised.
 * The caller that created the socket retains its original inherit flag on failure.
 * On success the creating GetCurrentThreadId is the sole operation thread.
 */
int interop_windows_socket_create(SOCKET connected_socket,
    const interop_windows_socket_config_t *config,
    interop_windows_socket_t **out_owner);

/** @brief Produce poll interests, at most 100ms wait, and a fresh poll cookie.
 * @param[in] owner Authentic live owner, used on create thread only.
 * @param[in] token Exact nonzero connection token.
 * @param[in] now_ms Nondecreasing caller monotonic time.
 * @param[out] out_poll Disjoint aligned output; unchanged on error.
 * @return Success or a local validation/thread/token/time error.
 * @note Caller performs WSAPoll using the copied socket/events/timeout and passes
 * its revents to exactly one drive call. This function performs no wait or I/O.
 */
int interop_windows_socket_poll_events(interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, interop_windows_socket_poll_t *out_poll);

/** @brief Read a stable terminal snapshot after successful native close.
 * @param[in] owner Authentic owner in TransportClosed, create thread only.
 * @param[in] token Exact connection token.
 * @param[out] out_work Disjoint aligned output; unchanged on error.
 * @return Success copies the terminal state; WrongPhase unless closesocket succeeded.
 * @note No peer call, timer change, native call, or event replay occurs.
 */
int interop_windows_socket_get_closed_snapshot(interop_windows_socket_t *owner,
    uint64_t token, interop_windows_socket_work_t *out_work);

/** @brief Advance timers and bounded nonblocking I/O from one poll snapshot.
 * @param[in,out] owner Authentic live owner; serialized create thread only.
 * @param[in] token Exact token; checked before state/time.
 * @param[in] poll_cookie Latest successful poll cookie, consumed on success.
 * @param[in] now_ms Nondecreasing monotonic milliseconds; call at least every 100ms.
 * @param[in] revents WSAPoll result bits from the matching snapshot.
 * @param[in] rx_byte_budget 0..8192 bytes.
 * @param[in] tx_byte_budget 0..65536 bytes.
 * @param[in] syscall_budget 0..64 combined recv/send attempts.
 * @param[out] frame_out Writable 65536-byte buffer, disjoint from other ranges.
 * @param[out] out_work Disjoint aligned output; unchanged on API error.
 * @return Success commits copied work; API errors preserve outputs.
 * @note One call reads at most one copied peer frame. Each positive send is
 * acknowledged exactly before any next send. It never ACKs or proves mapping release.
 */
int interop_windows_socket_drive(interop_windows_socket_t *owner, uint64_t token,
    uint64_t poll_cookie, uint64_t now_ms, uint32_t revents,
    size_t rx_byte_budget, size_t tx_byte_budget, uint32_t syscall_budget,
    uint8_t *frame_out, size_t frame_capacity,
    interop_windows_socket_work_t *out_work);

/** @brief Report completion of the explicit external mapping operation.
 * @param[in,out] owner Authentic owner in MappingWait, create thread only.
 * @param[in] token Exact connection token; @param[in] now_ms nondecreasing time.
 * @param[in] epoch Validated nonzero peer epoch; @param[in] layout_version validated layout.
 * @param[in] succeeded Exactly 0 or 1.
 * @param[out] out_event Disjoint aligned copied peer event; unchanged on error.
 * @return Success or MappingFailed after a reported unsuccessful guest initialization.
 * @note Guest caller initializes its mapping before this call; only success permits
 * Hello. Host caller may report only after a ValidateMapping event. No mapping is owned.
 */
int interop_windows_socket_mapping_result(interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, uint64_t epoch, uint32_t layout_version,
    uint32_t succeeded, interop_peer_event_t *out_event);

/** @brief Stop admission and begin one finite graceful TX drain.
 * @param[in,out] owner Authentic live owner; create thread only.
 * @param[in] token Exact connection token.
 * @param[in] now_ms Nondecreasing caller monotonic milliseconds.
 * @param[out] out_event Disjoint aligned copied event; unchanged on API error.
 * @return Success starts one absolute drain deadline; local validation or peer failure otherwise.
 * @note The first Draining transition fixes the deadline; later calls cannot extend it.
 * A completed drain closes only this socket transport and is not mapping release proof.
 */
int interop_windows_socket_begin_drain(interop_windows_socket_t *owner,
    uint64_t token, uint64_t now_ms, interop_peer_event_t *out_event);

/** @brief Immediately end admission and make one bounded close attempt.
 * @param[in,out] owner Authentic owner; create thread only.
 * @param[in] token Exact connection token.
 * @param[out] out_work Disjoint aligned copied result; unchanged on API error.
 * @return Success reports the terminal outcome; native close failures are represented
 * in work status/state and leave the owner available for explicit retry or handoff.
 * @note Successful native close reports transport loss once. Failure never does.
 */
int interop_windows_socket_abort(interop_windows_socket_t *owner,
    uint64_t token, interop_windows_socket_work_t *out_work);

/** @brief Retry exactly one close after a prior WSAEWOULDBLOCK result.
 * @param[in,out] owner Authentic ClosePendingState owner; create thread only.
 * @param[in] token Exact connection token.
 * @return Success after native close and one peer transport-lost notification;
 * ClosePending if WSAEWOULDBLOCK recurs or linger remains enabled; CloseIndeterminate
 * for another native close error; OsError if linger cannot be read.
 * @note No retry is permitted after any other close error. SO_LINGER must still
 * be disabled and no other thread may call WinSock on the SOCKET. One call makes
 * at most one closesocket attempt; the caller controls whether another call occurs.
 */
int interop_windows_socket_retry_close(interop_windows_socket_t *owner,
    uint64_t token);

/** @brief Transfer unresolved socket identity into a caller-owned quarantine record.
 * @param[in,out] owner_slot Aligned slot containing the authentic owner.
 * @param[in] token Exact connection token.
 * @param[out] quarantine_out Writable disjoint result.
 * @return Success only in ClosePendingState or CloseIndeterminateState.
 * @note quarantine_out must be a caller-initialized all-zero record. Copies only
 * SOCKET/error/classification/thread/token, then destroys peer
 * allocations, frees owner and NULLs owner_slot. It does not call closesocket,
 * authorize a future close/reuse, notify transport_lost, or establish detach proof.
 * Caller must retain the record through process lifetime or separate explicit
 * native resolution. Quarantine ownership is not a mapping epoch or host drain proof.
 */
int interop_windows_socket_quarantine_handoff(
    interop_windows_socket_t **owner_slot,
    uint64_t token,
    interop_windows_socket_quarantine_t *quarantine_out);

/** @brief Close a live socket once, release peer/owner after native close succeeds.
 * @param[in,out] owner_slot Aligned slot containing NULL or authentic owner.
 * @return Success NULLs slot; ClosePending/CloseIndeterminate retain it for retry or
 * quarantine handoff. Socket is cleared only after closesocket returns success.
 * @note No cleanup path retries automatically and no close error permits transport_lost.
 */
int interop_windows_socket_destroy(interop_windows_socket_t **owner_slot);

#ifdef __cplusplus
}
#endif
#endif
