/** @file daemon_guest_ready.h
 * @brief Serialized bounded guest-exec reachability probe, without interop authority.
 */
#ifndef WaddleDaemonGuestReadyH
#define WaddleDaemonGuestReadyH
#include "waddle/daemon_protocol.h"

/** @brief Caller-owned probe value; initialize once and never copy a live fd.
 * @note No allocation. Event-loop thread serializes every operation; cancel before free.
 */
typedef struct daemon_guest_ready_t {
    int probe_fd; /**< Owned NONBLOCK/CLOEXEC fd, -1 when absent. */
    int active; /**< One while a request owns this state. */
    uint32_t sequence; /**< Borrowed request's copied correlation number. */
    uint32_t cid; /**< Copied configured guest CID. */
    uint32_t port; /**< Copied configured guest-exec port, never replaced. */
    uint64_t deadline_ms; /**< Absolute monotonic request deadline. */
    uint64_t probe_deadline_ms; /**< Absolute current connect deadline. */
    uint64_t retry_at_ms; /**< Earliest permitted next attempt. */
} daemon_guest_ready_t;

/** @brief Initialize one caller-owned value; no OS calls or allocation.
 * @param[out] probe Non-null aligned writable uninitialized value.
 * @note Single owner; initializing a live value would leak its fd and is forbidden.
 */
void daemon_guest_ready_init(daemon_guest_ready_t *probe);

/** @brief Cancel a wait and relinquish its probe fd exactly once.
 * @param[in,out] probe Non-null authentic initialized value, owned by calling thread.
 * @note No allocation; close errors are diagnostic only and are never retried.
 */
void daemon_guest_ready_cancel(daemon_guest_ready_t *probe);

/** @brief Start a bounded wait without creating a socket or blocking.
 * @param[in,out] probe Non-null initialized inactive value, caller serialized.
 * @param[in] cid Configured CID, 3..UINT32_MAX-1.
 * @param[in] port Nonzero configured guest-exec port.
 * @param[in] timeout_ms Validated finite duration, 1..120000.
 * @param[in] now_ms Current monotonic milliseconds; must allow deadline addition.
 * @param[in] sequence Request correlation copied into probe.
 * @return 0 success, -1 errno EINVAL/EBUSY; all state unchanged on failure.
 */
int daemon_guest_ready_start(daemon_guest_ready_t *probe, uint32_t cid,
                             uint32_t port, uint32_t timeout_ms,
                             uint64_t now_ms, uint32_t sequence);

/** @brief Advance a wait by at most one nonblocking connect/completion attempt.
 * @param[in,out] probe Non-null authentic active value on its owning event-loop thread.
 * @param[in] running Nonzero only while the daemon subsystem is Running.
 * @param[in] now_ms Nondecreasing monotonic milliseconds.
 * @param[in] revents poll result for the current probe_fd, zero when not polled.
 * @param[out] response Non-null writable response disjoint from probe.
 * @return 1 completed with committed response, 0 pending/inactive with output unchanged.
 * @note No allocation or waits. Completion closes its fd before publishing output.
 */
int daemon_guest_ready_step(daemon_guest_ready_t *probe, int running,
                            uint64_t now_ms, short revents,
                            waddle_daemon_guest_ready_resp_t *response);
#endif
