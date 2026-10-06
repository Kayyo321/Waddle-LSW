/** @file venus_worker.h @brief Linux isolated receiver process ownership. */
#ifndef WaddleVenusWorkerH
/** @brief Include guard, no storage/ownership. */
#define WaddleVenusWorkerH
#include "venus_ring.h"
#include <sys/types.h>
/** @brief Child mapping descriptor after exec. */
#define VenusWorkerMappingFd 3
/** @brief Child nonblocking guest control stream after exec. */
#define VenusWorkerStreamFd 4
/** @brief Caller-owned process identity; all fields read-only to callers.
 * @note Zero initialize, never copy/use concurrently. Owns child/group until
 * reaped; destroy before reuse. No heap or retained mapping/socket ownership.
 */
typedef struct venus_worker_t {
    pid_t process_id; /**< Unreaped owned child/group, zero when none. */
    int terminating;  /**< Zero running, one TERM requested, two KILL requested. */
    int exited;       /**< Exit observed/reaped; status is valid unless -1. */
    int exit_status;  /**< Native wait status, -1 on external reaping violation. */
} venus_worker_t;
/** @brief Launch a trusted receiver executable in a fresh owned process group.
 * @param[out] worker Nonnull zero private record, unchanged on failure.
 * @param[in] path Nonnull trusted immutable absolute executable path; no shell.
 * @param[in] mapping_fd Borrowed regular mapping fd, retained/closed by caller.
 * @param[in] stream_fd Borrowed nonblocking SOCK_STREAM fd, retained by caller.
 * @return RingOk, RingInvalid for arguments/descriptors, RingCorrupt spawn failure.
 * @note Linux/session-controller-thread-only. Child argv includes --venus-worker;
 * fd 3/4 duplicate borrowed sources, all fd>=5 closed, standard streams inherited.
 * No other waiter/SIGCHLD handler may reap this child. Descendants must retain group.
 */
venus_ring_status_t venus_worker_create(venus_worker_t *worker, const char *path, int mapping_fd,
                                        int stream_fd);
/** @brief Nonblocking observe/clean group/reap, preserving child identity ordering.
 * @param[in,out] worker Nonnull owned live/exited record.
 * @return RingAgain running/interrupted, RingClosed exited, RingInvalid zero/null,
 * RingCorrupt OS/ownership failure; ECHILD clears unsafe identity without signaling.
 * @note Controller-thread-only; no allocation or blocking wait. On exit kills
 * descendants before reaping zombie so the process group ID cannot be reused.
 */
venus_ring_status_t venus_worker_poll(venus_worker_t *worker);
/** @brief Request bounded group shutdown and reap before clearing record.
 * @param[in,out] worker Nullable owned record; zero/exited destroy is a no-op.
 * @param[in] timeout_ms Whole shutdown budget, 1..60000ms; TERM then KILL.
 * @return RingOk cleared, RingInvalid bad budget, RingTimeout/Corrupt retaining
 * unreaped ownership for later retry. Never abandon a retained process identity.
 * @note Controller-thread-only, one-ms sleeps, no blocking waitpid. Cannot repair
 * a stuck kernel task; mapping/socket release is caller-owned after worker stop.
 */
venus_ring_status_t venus_worker_destroy(venus_worker_t *worker, uint32_t timeout_ms);
#endif
