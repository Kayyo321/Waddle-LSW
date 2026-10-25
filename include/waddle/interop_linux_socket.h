#ifndef WaddleInteropLinuxSocketH
#define WaddleInteropLinuxSocketH
/**
 * @file interop_linux_socket.h
 * @brief Pull-driven owner for one connected Linux stream socket.
 *
 * It owns an
 * already-connected nonblocking CLOEXEC SOCK_STREAM fd only after create
 * succeeds. It does not own mapping or resource-release proof.
 */
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include <waddle/interop_peer.h>
#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque owner returned by create; caller must not copy or forge it. */
typedef struct interop_linux_socket_t interop_linux_socket_t;

/** @brief Fixed RX staging bound; the peer ABI owns partial-frame assembly. */
#define InteropLinuxSocketRxCarryBytes 8192u
/** @brief Maximum accepted TX bytes per drive call. */
#define InteropLinuxSocketMaxTxBudgetBytes 65536u
/** @brief Maximum accepted RX bytes per drive call. */
#define InteropLinuxSocketMaxRxBudgetBytes 8192u
/** @brief Maximum combined recv/send attempts per drive call. */
#define InteropLinuxSocketMaxSyscallBudget 64u
/** @brief Maximum combined peer/helper steps per drive call. */
#define InteropLinuxSocketMaxInternalStepsPerDrive 64u
/** @brief Upper bound for caller-configured graceful-drain interval. */
#define InteropLinuxSocketMaxDrainTimeoutMs 60000u
/** @brief Required maximum time between calls that advance peer timers. */
#define InteropLinuxSocketMaxDriveIntervalMs 100u
/** @brief Required capacity of caller-owned event-frame output. */
#define InteropLinuxSocketFrameCapacity 65536u

/** @brief Result of local API validation or owner operation. */
typedef enum interop_linux_socket_result_t {
    InteropLinuxSocketSuccess = 0, /**< Operation completed; inspect outputs. */
    InteropLinuxSocketInvalidArgument = 1, /**< Pointer/range/value/alias invalid. */
    InteropLinuxSocketOutOfMemory = 2, /**< No descriptor ownership transferred. */
    InteropLinuxSocketWrongToken = 3, /**< Stale connection token; no mutation. */
    InteropLinuxSocketWrongThread = 4, /**< Not the owner thread; no mutation. */
    InteropLinuxSocketTimeWentBackwards = 5, /**< Repair monotonic time and retry. */
    InteropLinuxSocketWrongPhase = 6, /**< Operation invalid for owner/peer phase. */
    InteropLinuxSocketStalePoll = 7, /**< Poll cookie is not the current snapshot. */
    InteropLinuxSocketPollCookieExhausted = 8, /**< Cookie counter reached UINT64_MAX. */
    InteropLinuxSocketClosed = 9, /**< Operation requires a live transport. */
    InteropLinuxSocketPeerFailure = 10, /**< Unexpected peer ABI failure; owner terminalizes. */
    InteropLinuxSocketCloseDiagnostic = 11, /**< Destroy completed; close failed, slot is NULL. */
    InteropLinuxSocketMappingFailed = 12, /**< Guest mapping failure; wait remains retryable. */
    InteropLinuxSocketInvalidFrame = 13, /**< Feature frame malformed or identifier class violated; nothing queued. */
    InteropLinuxSocketUnsupportedFrame = 14, /**< Type not legal for this entry point or producer; nothing queued. */
    InteropLinuxSocketFeatureNotNegotiated = 15, /**< Facility absent from the negotiated set; nothing queued. */
    InteropLinuxSocketQueueFull = 16, /**< Data-class admission full; retry after TX drains. */
    InteropLinuxSocketRequestIdsFull = 17, /**< 4032 feature identifiers live; nothing reserved or queued. */
    InteropLinuxSocketUnknownRequestId = 18, /**< Release target is not a live feature identifier; unchanged. */
    InteropLinuxSocketRequestRetired = 19 /**< Transport loss retired every identifier; benign release result. */
} interop_linux_socket_result_t;

/** @brief Owner lifecycle state; Closed is a stable terminal snapshot. */
typedef enum interop_linux_socket_state_t {
    InteropLinuxSocketActive = 1, /**< Normal handshake/ready processing. */
    InteropLinuxSocketMappingWait = 2, /**< Guest initial mapping or host validation pending. */
    InteropLinuxSocketDraining = 3, /**< Admission ended; bounded TX flush only. */
    InteropLinuxSocketTransportClosed = 4 /**< fd closed once; owner still destroyable. */
} interop_linux_socket_state_t;

/** @brief Operational outcome reported by drive, not an API-call error. */
typedef enum interop_linux_socket_work_status_t {
    InteropLinuxSocketWorkIdle = 0, /**< No event, I/O, or terminal condition. */
    InteropLinuxSocketWorkProgress = 1, /**< Bytes moved or peer clock advanced. */
    InteropLinuxSocketWorkTransportClosed = 2, /**< EOF/error/abort/drain completed. */
    InteropLinuxSocketWorkPeerFailure = 3 /**< Peer invariant failed; transport closed. */
} interop_linux_socket_work_status_t;

/** @brief First preserved reason for a committed transport-close outcome. */
typedef enum interop_linux_socket_terminal_reason_t {
    InteropLinuxSocketTerminalNone = 0, /**< Transport has not closed. */
    InteropLinuxSocketTerminalEof = 1, /**< Generic recv EOF; no framing claim. */
    InteropLinuxSocketTerminalSocketError = 2, /**< Fatal recv/send/poll socket error. */
    InteropLinuxSocketTerminalPollInvalid = 3, /**< poll reported POLLNVAL. */
    InteropLinuxSocketTerminalPeerAbort = 4, /**< Peer event required immediate transport abort. */
    InteropLinuxSocketTerminalDrainDeadline = 5, /**< Graceful flush deadline elapsed. */
    InteropLinuxSocketTerminalLocalAbort = 6, /**< Caller requested immediate abort. */
    InteropLinuxSocketTerminalPeerFailure = 7, /**< Unexpected peer ABI invariant result. */
    InteropLinuxSocketTerminalDestroyed = 8, /**< Explicit destroy initiated closure. */
    InteropLinuxSocketTerminalDrainComplete = 9 /**< Permitted TX queue emptied in Draining. */
} interop_linux_socket_terminal_reason_t;

/** @brief Borrowed peer-create configuration plus bounded drain policy.
 * @param[in] peer_config Aligned accessible config copied by peer create.
 * @param[in] drain_timeout_ms Graceful Ready-drain interval, 1..MaxDrainTimeoutMs.
 * @note Not retained after create; caller retains the config storage.
 */
typedef struct interop_linux_socket_config_t {
    interop_peer_config_t peer_config; /**< Pure protocol peer configuration. */
    uint32_t drain_timeout_ms; /**< Finite Ready-drain interval in milliseconds. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_linux_socket_config_t;

/** @brief Poll request and timer guidance for one cookie snapshot.
 * @param[out] fd Borrowed owner descriptor for this poll snapshot; -1 when closed.
 * @param[out] events Requested poll bits; may include POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL.
 * @param[out] timeout_ms Caller poll timeout, always 0..MaxDriveIntervalMs.
 * @param[out] immediate_work Nonzero means call drive without blocking.
 * @param[out] poll_cookie Nonzero single-use generation for matching drive.
 */
typedef struct interop_linux_socket_poll_t {
    int32_t fd; /**< Borrowed numeric fd; valid only until any owner operation. */
    uint32_t events; /**< Requested poll bits; numeric Linux poll constants. */
    uint32_t timeout_ms; /**< Recommended timeout; never above 100 ms. */
    uint32_t immediate_work; /**< Exactly 0 or 1. */
    uint32_t reserved_zero; /**< Always zero. */
    uint32_t reserved_zero_2; /**< Always zero; occupies the old implicit alignment padding. */
    uint64_t poll_cookie; /**< Latest nonzero single-use snapshot generation. */
} interop_linux_socket_poll_t;

/** @brief One drive result; frame bytes, when any, are in caller output.
 * @param[out] state Current owner lifecycle state.
 * @param[out] status Operational outcome; socket failures are not API errors.
 * @param[out] rx_bytes Number of bytes read from fd during this call.
 * @param[out] tx_bytes Number of bytes positively accepted by send during this call, including before an ACK invariant failure.
 * @param[out] syscalls Number of recv/send attempts charged to the budget.
 * @param[out] internal_steps Pure peer/helper operations, bounded by MaxInternalStepsPerDrive.
 * @param[out] terminal_reason First terminal cause or None.
 * @param[out] system_errno First preserved errno for terminal system failure, else 0.
 * @param[out] event One newly consumed peer event; None when absent.
 * @param[out] diagnostic_event Stable terminal snapshot only; never repeats a FeatureFrame.
 */
typedef struct interop_linux_socket_work_t {
    uint32_t state; /**< interop_linux_socket_state_t. */
    uint32_t status; /**< interop_linux_socket_work_status_t. */
    size_t rx_bytes; /**< Successful recv bytes this call. */
    size_t tx_bytes; /**< Positive OS send bytes, even if a later ACK invariant fails. */
    uint32_t syscalls; /**< recv/send attempts this call. */
    uint32_t internal_steps; /**< Peer/helper operations this call. */
    uint32_t terminal_reason; /**< interop_linux_socket_terminal_reason_t. */
    int32_t system_errno; /**< Preserved terminal errno or zero. */
    interop_peer_event_t event; /**< At most one newly consumed peer event. */
    interop_peer_event_t diagnostic_event; /**< Stable terminal snapshot, if any. */
} interop_linux_socket_work_t;

/** @brief Transfer a validated connected descriptor into a new owner.
 * @param[in] connected_fd Nonnegative, connected, nonblocking CLOEXEC stream fd.
 * @param[in] config Non-NULL aligned readable config, disjoint from out_owner.
 * @param[out] out_owner Non-NULL aligned NULL-valued slot, disjoint from config.
 * @return Success transfers fd ownership; every error leaves fd with caller and slot NULL.
 * @note Captures calling pthread as sole owner thread. Does not alter fd flags.
 *       Guest role starts in MappingWait and requires caller mapping_result before Hello.
 */
int interop_linux_socket_create(int connected_fd, const interop_linux_socket_config_t *config, interop_linux_socket_t **out_owner);

/** @brief Produce poll interests and bounded timeout guidance.
 * @param[in] owner Authentic live owner returned by create; owner-thread only.
 * @param[in] token Exact nonzero configured connection token.
 * @param[in] now_ms Nondecreasing caller monotonic milliseconds.
 * @param[out] out_poll Non-NULL aligned output, disjoint from owner; unchanged on error.
 * @return Success or local error; successful call records time and issues a fresh cookie.
 * @note A successful cookie is consumed by one drive call or invalidated by any state change.
 */
int interop_linux_socket_poll_events(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms, interop_linux_socket_poll_t *out_poll);

/** @brief Read a stable terminal snapshot without a poll cookie or I/O.
 * @param[in] owner Authentic live owner in TransportClosed; owner-thread only.
 * @param[in] token Exact connection token.
 * @param[out] out_work Non-NULL aligned output, disjoint from owner; unchanged on error.
 * @return Success copies terminal diagnostic; WrongPhase if transport is not closed.
 * @note No timestamp, syscall, peer event replay, or state mutation.
 */
int interop_linux_socket_get_closed_snapshot(interop_linux_socket_t *owner, uint64_t token, interop_linux_socket_work_t *out_work);

/** @brief Advance peer timers and bounded socket I/O from one poll result.
 * @param[in,out] owner Authentic live owner; all calls serialized on create thread.
 * @param[in] token Exact connection token; checked before time/state access.
 * @param[in] poll_cookie Latest successful poll cookie, consumed on success.
 * @param[in] now_ms Nondecreasing monotonic time; drive no less often than every 100 ms.
 * @param[in] revents Bits returned by poll for this snapshot; supported bits only.
 * @param[in] rx_byte_budget 0..MaxRxBudgetBytes.
 * @param[in] tx_byte_budget 0..MaxTxBudgetBytes.
 * @param[in] syscall_budget 0..MaxSyscallBudget combined recv/send attempts.
 * @param[out] frame_out Caller-owned accessible 65536-byte buffer, disjoint from all other ranges.
 * @param[in] frame_capacity Must be at least InteropLinuxSocketFrameCapacity.
 * @param[out] out_work Non-NULL aligned work output, disjoint from frame output.
 * @return Success commits an operational result into outputs; local API errors preserve all outputs.
 * @note Positive send counts are ACKed immediately and exactly before another send.
 */
int interop_linux_socket_drive(interop_linux_socket_t *owner, uint64_t token, uint64_t poll_cookie, uint64_t now_ms, uint32_t revents, size_t rx_byte_budget, size_t tx_byte_budget, uint32_t syscall_budget, uint8_t *frame_out, size_t frame_capacity, interop_linux_socket_work_t *out_work);

/** @brief Admit one complete outbound feature frame that carries no request identifier.
 * @param[in,out] owner Authentic live owner on the creating thread; changed only on success.
 * @param[in] token Exact nonzero connection token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[in] bytes Borrowed complete frame, NULL only when length is zero (then InvalidFrame); not aliased
 *        with the owner or out_event. @param[in] length Exact frame size.
 * @param[out] out_event Non-NULL aligned output disjoint from owner and bytes; unchanged on API error.
 *        None after queueing; Draining when the Ready deadline expired (frame not queued, owner drains).
 * @return Success, or the operation result. Peer admission errors map one-to-one to the InvalidFrame,
 *         UnsupportedFrame, FeatureNotNegotiated, QueueFull, WrongPhase and related results. Every error
 *         leaves the owner, peer and queue unchanged.
 * @note Successful admission invalidates the outstanding poll snapshot so the next poll requests POLLOUT.
 * @thread_safety Owner-thread only, serialized with every other owner call.
 */
int interop_linux_socket_enqueue_feature(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms, const uint8_t *bytes, size_t length, interop_peer_event_t *out_event);

/** @brief Atomically reserve a feature request identifier and admit one Request frame.
 * @param[in,out] owner Authentic live owner on the creating thread; changed only on success.
 * @param[in] token Exact nonzero connection token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[in] bytes Borrowed complete request frame with zero header identifier; NULL only with length zero.
 * @param[in] length Exact frame size. @param[out] out_request_id Non-NULL aligned output disjoint from owner,
 *        bytes and out_event; written only on success (zero when the deadline drained the connection).
 * @param[out] out_event Non-NULL aligned output disjoint from owner, bytes and out_request_id; written only on success.
 * @return Success or a mapped peer result. RequestIdsFull and QueueFull leave the identifier cursor unchanged.
 * @thread_safety Owner-thread only, serialized with every other owner call.
 */
int interop_linux_socket_enqueue_request(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms, const uint8_t *bytes, size_t length, uint32_t *out_request_id, interop_peer_event_t *out_event);

/** @brief Release one feature-owned request identifier after its response or abandonment.
 * @param[in,out] owner Authentic owner on the creating thread; identifier namespace changes only on success.
 * @param[in] token Exact nonzero connection token. @param[in] request_id Identifier from enqueue_request.
 * @return Success; UnknownRequestId for any non-feature or released identifier; RequestRetired after transport
 *         loss (benign); InvalidArgument for zero; WrongToken for a stale token.
 * @note Allowed after transport closure; the peer decides whether identifiers remain to release.
 * @thread_safety Owner-thread only.
 */
int interop_linux_socket_release_request(interop_linux_socket_t *owner, uint64_t token, uint32_t request_id);

/** @brief Complete the external mapping operation requested by ValidateMapping.
 * @param[in,out] owner Authentic owner in MappingWait; owner-thread only.
 * @param[in] token Exact connection token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[in] epoch Validated epoch. @param[in] layout_version Actual validated layout.
 * @param[in] succeeded Exactly 0 or 1; mapping itself remains externally owned.
 * @param[out] out_event Non-NULL aligned output, disjoint from owner; unchanged on API error.
 * @return Success reports the peer transition; MappingFailed is expected for guest failure.
 * @note Guest starts in MappingWait after create without a ValidateMapping event; caller
 *       initializes the guest mapping and reports it here. Host may call only after event.
 */
int interop_linux_socket_mapping_result(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms, uint64_t epoch, uint32_t layout_version, uint32_t succeeded, interop_peer_event_t *out_event);

/** @brief Stop admission and begin bounded transport drain.
 * @param[in,out] owner Authentic live owner; owner-thread only.
 * @param[in] token Exact connection token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] out_event Non-NULL aligned output, disjoint from owner; unchanged on API error.
 * @return Success starts a saturating deadline only on first Draining transition.
 * @note Success invalidates outstanding poll cookie.
 * @note Deadline equality is expired; this never proves mapping or producer release.
 */
int interop_linux_socket_begin_drain(interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms, interop_peer_event_t *out_event);

/** @brief Abort transport immediately without graceful TX flushing.
 * @param[in,out] owner Authentic owner; owner-thread only.
 * @param[in] token Exact connection token.
 * @param[out] out_work Non-NULL aligned output, disjoint from owner; unchanged on API error.
 * @return Success commits closure and a stable snapshot; the first terminal cause is retained.
 * @note No cookie/time required; invalidates any poll cookie and never proves resource release.
 */
int interop_linux_socket_abort(interop_linux_socket_t *owner, uint64_t token, interop_linux_socket_work_t *out_work);

/** @brief Close transport once and release peer/owner allocations.
 * @param[in,out] owner_slot Non-NULL aligned slot containing NULL or authentic owner.
 * @return Success NULLs slot; CloseDiagnostic also NULLs slot after full cleanup.
 * @note Other errors retain the allocation and slot for retry; transport may already be closed.
 *       Serialized; close is attempted once, never retried on EINTR.
 * @note CloseDiagnostic sets errno to the one close error; fd ownership is still relinquished.
 */
int interop_linux_socket_destroy(interop_linux_socket_t **owner_slot);

#if defined(__cplusplus)
static_assert(sizeof(interop_linux_socket_config_t) == 56, "socket config ABI");
static_assert(sizeof(interop_linux_socket_poll_t) == 32, "socket poll ABI");
static_assert(sizeof(interop_linux_socket_work_t) == 152, "socket work ABI");
#else
_Static_assert(sizeof(interop_linux_socket_config_t) == 56, "socket config ABI");
_Static_assert(sizeof(interop_linux_socket_poll_t) == 32, "socket poll ABI");
_Static_assert(sizeof(interop_linux_socket_work_t) == 152, "socket work ABI");
#endif
#ifdef __cplusplus
}
#endif
#endif
