/** @file venus_export.h @brief Bounded worker presentation allocation leases. */
#ifndef WaddleVenusExportH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusExportH
#include "venus_frame.h"
#include "venus_receiver.h"
/** @brief Maximum outstanding worker frame leases, including unconsumed completions. */
#define VenusExportSlots 3u
/** @brief Private caller-owned lease, naturally eight-byte aligned, no FD ownership.
 * @note Sole worker thread. Empty release.frame denotes a free slot.
 */
typedef struct venus_export_lease_t {
    venus_release_t release;  /**< Exact completion identity/status; initially status Ok. */
    uint32_t resource_ids[4]; /**< Registered allocations retained until guest consumption. */
    uint32_t completed; /**< One after authenticated completion, zero while compositor owns. */
} venus_export_lease_t;
/** @brief Caller-owned worker state, initially zero, no heap/persistent FD ownership.
 * @note Sole worker thread; receiver/socket/controller identity borrowed until free.
 * Never copy live state or serialize native padding. Symmetric init/free required.
 */
typedef struct venus_export_t {
    venus_receiver_t *receiver;   /**< Borrowed live worker receiver; NULL denotes uninitialized. */
    int socket_fd;                /**< Borrowed prepared nonblocking CLOEXEC seqpacket endpoint. */
    int32_t controller_pid;       /**< Positive retained controller identity, never reused. */
    uint64_t context;             /**< Nonzero exact controller context binding. */
    uint64_t last_frame;          /**< Last successfully queued frame; IDs never wrap/reuse. */
    venus_ring_status_t terminal; /**< Sticky native/protocol failure or RingOk. */
    venus_export_lease_t leases[VenusExportSlots]; /**< Fixed outstanding ownership records. */
} venus_export_t;
/** @brief Initialize zeroed private worker export state with borrowed resources.
 * @param[in,out] owner Nonnull initially-zero private state, unchanged on failure.
 * @param[in] receiver Nonnull live borrowed receiver, retained until free.
 * @param[in] socket_fd Borrowed prepared native endpoint.
 * @param[in] controller_pid Positive retained controller PID.
 * @param[in] context Nonzero trusted context identity.
 * @return RingOk or RingInvalid NULL/already-live/identity/socket error.
 * @note Sole worker thread; no allocation or resource acquisition.
 */
venus_ring_status_t venus_export_init(venus_export_t *owner, venus_receiver_t *receiver,
                                      int socket_fd, int32_t controller_pid, uint64_t context);
/** @brief Clear private state after publication stops and receiver teardown is guaranteed.
 * @param[in,out] owner Nullable private state; zeroed, idempotent.
 * @note Sole worker thread. Caller must stop service/abandon old context and destroy
 * receiver; this does not free remote allocations or revoke compositor buffers.
 */
void venus_export_free(venus_export_t *owner);
/** @brief Export and queue a bounded private frame after explicit GPU retirement.
 * @param[in,out] owner Nonnull live sole-thread owner.
 * @param[in] bytes Nonnull immutable private frame bytes[length], borrowed for call.
 * @param[in] length Exactly VenusFrameBytes, parsed by bounded Zig codec.
 * @param[in] timeline Explicit GPU timeline 1..63.
 * @param[in] fence Nonzero previously issued fence on timeline.
 * @return RingOk queued lease; Again full/busy/pending fence/socket pressure;
 * Invalid local/schema/order/context; receiver quota/native terminal result otherwise.
 * @note Closes all temporary exports on every path. On failure no lease/ID advances.
 * Caller must keep allocations immutable/live until release consumption.
 */
venus_ring_status_t venus_export_submit(venus_export_t *owner, const void *bytes, size_t length,
                                        uint32_t timeline, uint64_t fence);
/** @brief Pump at most three authenticated reverse-channel packets without blocking.
 * @param[in,out] owner Nonnull live sole-thread state.
 * @return RingOk drained/no packet; Invalid NULL/uninitialized; sticky native
 * Closed/Corrupt, including unknown or duplicate frame acknowledgements.
 * @note Retains lease/resource identities after completion until guest consumption.
 */
venus_ring_status_t venus_export_pump(venus_export_t *owner);
/** @brief Consume an authenticated completion exactly once.
 * @param[in,out] owner Nonnull live sole-thread state.
 * @param[in] frame Nonzero outstanding frame ID.
 * @param[out] release Nonnull disjoint private output, zeroed on failure.
 * @return RingOk copied/consumed completion, Again pending, Invalid absent/local,
 * sticky Closed/Corrupt native failure. Pumps reverse packets before lookup.
 * @note No FD acquisition or Vulkan resource free; guest owns subsequent reuse.
 */
venus_ring_status_t venus_export_take(venus_export_t *owner, uint64_t frame,
                                      venus_release_t *release);
/** @brief Check whether an outstanding lease prohibits freeing a registration.
 * @param[in] owner Nullable immutable private state.
 * @param[in] resource_id Registered resource identity.
 * @return Nonzero busy, zero unreferenced/NULL/uninitialized.
 * @note Sole worker thread; no allocation, mutation or retained pointer.
 */
int venus_export_resource_busy(const venus_export_t *owner, uint32_t resource_id);
#endif
