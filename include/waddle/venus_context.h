/** @file venus_context.h @brief Bounded process-isolated Venus context controller. */
#ifndef WaddleVenusContextH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusContextH
#include "venus_worker.h"
/** @brief Maximum simultaneous isolated contexts, fixed controller storage. */
#define VenusContextMaxCount 8u
/** @brief Controller-owned slot, read-only to callers; natural native alignment.
 * @note No heap pointers. Worker and duplicate fd remain owned until joined.
 */
typedef struct venus_context_slot_t {
    venus_worker_t worker; /**< Solely owned child/group; never externally reaped. */
    uint64_t identity;     /**< Nonzero live handle, zero when free. */
    uint32_t capacity;     /**< Validated immutable ring capacity/offset snapshot. */
    uint64_t bytes;        /**< Charged mapping extent, immutable while live. */
    dev_t device;          /**< Retained mapping's device identity. */
    ino_t inode;           /**< Retained mapping's inode identity. */
    int mapping_fd;        /**< Owned CLOEXEC duplicate, closed after worker join. */
} venus_context_slot_t;
/** @brief Caller-owned fixed manager, zero before init; fields read-only.
 * @note Sole controller thread, no concurrent calls/copies. No heap allocation.
 * Owns slots until free succeeds; caller mappings/streams are never released.
 * Handles belong to one init/free lifetime; discard all before reinitializing.
 */
typedef struct venus_context_manager_t {
    venus_context_slot_t slots[VenusContextMaxCount]; /**< Independent worker owners. */
    uint64_t next_identity; /**< Monotonic handles; zero after exhaustion/free. */
    uint64_t byte_limit;    /**< Trusted aggregate mapping budget. */
    uint64_t live_bytes;    /**< Exact charged sum across live slots. */
    uint32_t count_limit;   /**< Trusted context ceiling, 1..8; zero uninitialized. */
    uint32_t live_count;    /**< Occupied slots, including exited until reaped. */
} venus_context_manager_t;
/** @brief Initialize bounded independent context policy, without allocation.
 * @param[out] manager Nonnull zero caller-owned record, unchanged on failure.
 * @param[in] count Trusted simultaneous context limit, 1..8.
 * @param[in] bytes Trusted aggregate mapping ceiling, 4096..8GiB.
 * @return RingOk or RingInvalid for null/live/out-of-range policy.
 * @note Sole controller thread; ownership ends only after free succeeds.
 */
venus_ring_status_t venus_context_manager_init(venus_context_manager_t *manager, uint32_t count,
                                               uint64_t bytes);
/** @brief Start an isolated renderer with a unique fresh mapping.
 * @param[in,out] manager Nonnull initialized exclusive controller record.
 * @param[in] path Nonnull borrowed trusted absolute worker executable path.
 * @param[in] mapping_fd Borrowed fixed-size initialized empty region fd, 4KiB..1GiB.
 * @param[in] stream_fd Borrowed connected nonblocking stream; exclusive worker peer.
 * @param[out] identity Nonnull disjoint private handle, zero on failure.
 * @return RingOk; RingInvalid local/duplicate/ABI; RingLimit count/bytes/IDs;
 * RingCorrupt mapping/dup/spawn failure. No slot/budget change on failure.
 * @note Controller-thread-only. Owns a duplicate mapping fd and child on success.
 * Caller retains mmap/original descriptors, closes host stream copy after launch,
 * and retains fixed backing until destroy/poll releases the worker.
 */
venus_ring_status_t venus_context_create(venus_context_manager_t *manager, const char *path,
                                         int mapping_fd, int stream_fd, uint64_t *identity);
/** @brief Observe one worker without blocking; reap exit and refund slot/budget.
 * @param[in,out] manager Nonnull initialized exclusive controller record.
 * @param[in] identity Nonzero live handle, never a slot index.
 * @return RingAgain running, RingClosed joined/closed/refunded, RingInvalid stale;
 * RingCorrupt OS/join/ring-closure failure; unreaped ownership retained for retry.
 * @note Controller-thread-only, no heap allocation. Closes both validated rings
 * after join, releases duplicate fd; no other context is touched.
 */
venus_ring_status_t venus_context_poll(venus_context_manager_t *manager, uint64_t identity);
/** @brief Shut down and join one context within its independent budget.
 * @param[in,out] manager Nonnull initialized exclusive controller record.
 * @param[in] identity Nonzero live handle; stale/zero rejected.
 * @param[in] timeout_ms TERM/KILL/reap budget, 1..60000ms.
 * @return RingOk joined/closed/refunded, RingInvalid stale/bad budget;
 * RingTimeout/Corrupt unreaped ownership retained. Ring closure errors release
 * joined ownership and return RingCorrupt; caller also closes its ring views.
 * @note Sole controller thread; borrowed original descriptors/mmap stay owned
 * by caller. Other contexts and handles remain valid.
 */
venus_ring_status_t venus_context_destroy(venus_context_manager_t *manager, uint64_t identity,
                                          uint32_t timeout_ms);
/** @brief Release every context, preserving failed ownership for a later retry.
 * @param[in,out] manager Nullable zero/live exclusive record; zero is a no-op.
 * @param[in] timeout_ms Per-context shutdown budget, 1..60000ms.
 * @return RingOk fully zeroed; RingInvalid budget; first shutdown/closure error
 * otherwise. Successful slots release even when another slot fails.
 * @note Controller-thread-only; at most count times budget. Never abandon a
 * retained child/fd, never close borrowed original descriptors or unmap them.
 */
venus_ring_status_t venus_context_manager_free(venus_context_manager_t *manager,
                                               uint32_t timeout_ms);
#endif
