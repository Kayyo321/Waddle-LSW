/**
 * @file linux_session_owner.h
 * @brief Own per-device Linux session leases and one bounded AF_VSOCK admission.
 *
 * The owner retains the per-device session and AV command locks. It may transfer
 * one completed AF_VSOCK stream to the existing Linux socket owner. It does not
 * own mappings, manifests, Wayland buffers, or external teardown proof.
 */
#ifndef WaddleInteropLinuxSessionOwnerH
#define WaddleInteropLinuxSessionOwnerH

#include <stddef.h>
#include <stdint.h>
#include "daemon_device.h"
#include <waddle/interop_linux_socket.h>
#include <waddle/interop_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque owner; never copy, forge, or free this object directly. */
typedef struct interop_linux_session_owner_t interop_linux_session_owner_t;

/** @brief Result of a session lease or one-shot VSOCK operation. */
typedef enum interop_linux_session_result_t {
    InteropLinuxSessionSuccess = 0, /**< Operation completed; inspect outputs. */
    InteropLinuxSessionInvalidArgument = 1, /**< Invalid, overlapping, or malformed input. */
    InteropLinuxSessionOutOfMemory = 2, /**< Allocation failed; no lease was published. */
    InteropLinuxSessionBusy = 3, /**< Per-device session lock is already held. */
    InteropLinuxSessionGuestPortConflict = 4, /**< Guest-exec port collides with 5002. */
    InteropLinuxSessionCancelled = 5, /**< Caller cancellation before definite admission. */
    InteropLinuxSessionTimedOut = 6, /**< Bounded connect deadline expired. */
    InteropLinuxSessionConnectError = 7, /**< VSOCK failed before or during admission. */
    InteropLinuxSessionPeerCreateError = 8, /**< Connected peer owner could not be created. */
    InteropLinuxSessionPeerAlreadyCreated = 9, /**< This lease already owns a peer. */
    InteropLinuxSessionSocketOwnerNotDestroyed = 10, /**< Peer owner still exists. */
    InteropLinuxSessionCloseDiagnostic = 11, /**< Lease release completed with close error. */
    InteropLinuxSessionWrongThread = 12, /**< Operation called outside creating pthread. */
    InteropLinuxSessionAvBusy = 13, /**< AV command singleton is already held. */
    InteropLinuxSessionBadState = 14, /**< Owner state does not permit this operation. */
    InteropLinuxSessionOsError = 15, /**< System operation failed; errno is preserved. */
    InteropLinuxSessionClockError = 16, /**< Monotonic clock failed. */
    InteropLinuxSessionAttemptConsumed = 17 /**< A prior attempt may have reached guest. */
} interop_linux_session_result_t;

/** @brief Borrowed immutable facts copied and validated by acquire. */
typedef struct interop_linux_session_config_t {
    const device_info_t *device; /**< Selected profile; readable through acquire only. */
    const uint8_t *agent_version; /**< SemVer2 bytes; copied into owner on acquire. */
    size_t agent_version_bytes; /**< Version length, 5..32 bytes. */
    uint64_t connection_token; /**< Nonzero stale-callback token; not authentication. */
    uint64_t local_capabilities; /**< Capability subset of InteropCapabilityMask. */
    uint32_t drain_timeout_ms; /**< Existing socket-owner drain interval, 1..60000. */
    uint32_t reserved_zero; /**< Must be zero. */
} interop_linux_session_config_t;

/** @brief Acquire session then AV leases for a selected validated profile.
 * @param[in] config Aligned readable initialized config and nested device/version ranges.
 * @param[out] out_owner Aligned writable NULL-valued slot, disjoint from all inputs.
 * @return Success; InvalidArgument for malformed input; GuestPortConflict for port 5002;
 *         OutOfMemory for wrapper allocation failure; Busy/AvBusy for lease contention;
 *         OsError for filesystem or lock failures. OS errno is preserved.
 * @note Copies all retained facts. The creating pthread owns all subsequent operations.
 *       Port 5002 is rejected before allocation or filesystem operations.
 */
interop_linux_session_result_t interop_linux_session_acquire(
    const interop_linux_session_config_t *config,
    interop_linux_session_owner_t **out_owner);

/** @brief Make the owner's single bounded connection attempt and transfer its peer.
 * @param[in,out] owner Authentic acquired owner on its creating pthread.
 * @param[in] connect_timeout_ms One finite attempt, 1..2000 milliseconds.
 * @param[in] cancel_fd -1 disables cancellation; otherwise a stable caller-owned pollable fd.
 * @return Success; InvalidArgument, WrongThread, PeerAlreadyCreated, AttemptConsumed,
 *         Cancelled, TimedOut, ConnectError, ClockError, or PeerCreateError as applicable.
 *         A definite connection refusal is retryable; ambiguous/post-accept outcomes consume
 *         the attempt. OS errno is preserved when an OS operation fails.
 * @note Never reads, writes, or closes cancel_fd. The connected fd transfers only when
 *       interop_linux_socket_create succeeds; post-connect failures consume the attempt.
 */
interop_linux_session_result_t interop_linux_session_connect_peer(
    interop_linux_session_owner_t *owner,
    uint32_t connect_timeout_ms,
    int cancel_fd);

/** @brief Return the borrowed socket owner while the calling pthread still owns it.
 * @param[in] owner Authentic acquired owner.
 * @return Borrowed peer owner, or NULL when absent/wrong-thread; wrong thread sets EPERM.
 * @note Caller serializes all socket-owner operations and must not destroy through this pointer.
 */
interop_linux_socket_t *interop_linux_session_socket_owner(
    const interop_linux_session_owner_t *owner);

/** @brief Destroy the peer owner without releasing either singleton lease.
 * @param[in,out] owner Authentic acquired owner; all operations remain on its pthread.
 * @return Success when absent or destroyed; InvalidArgument/WrongThread for local errors;
 *         CloseDiagnostic if destruction completed but close failed; BadState if destruction
 *         cannot safely relinquish the peer owner.
 * @note Unexpected failures preserve owner state so the caller can retry safely.
 */
interop_linux_session_result_t interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner);

/** @brief Release session and AV leases after caller-established external quiescence.
 * @param[in,out] owner_slot Aligned owner slot, disjoint from owner storage; set NULL on release.
 * @return Success; InvalidArgument, WrongThread, or SocketOwnerNotDestroyed without release;
 *         CloseDiagnostic after all local ownership is relinquished despite close errors.
 * @note Requires peer destruction and completed external native/mapping teardown. This API
 *       performs no mapping release or proof check. Close is never retried after EINTR.
 */
interop_linux_session_result_t interop_linux_session_release(
    interop_linux_session_owner_t **owner_slot);

#ifdef __cplusplus
}
#endif
#endif
