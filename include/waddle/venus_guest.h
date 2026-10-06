/** @file venus_guest.h @brief Negotiated guest command/resource/fence frontend. */
#ifndef WaddleVenusGuestH
/** @brief Include guard; no ownership or storage. */
#define WaddleVenusGuestH
#include "venus_capabilities.h"
#include "venus_rpc.h"
/** @brief Caller-owned sole-submission-thread frontend; never copy live records.
 * @note No allocation; borrows RPC/channel/scratch/mapping until free. Immutable
 * capabilities advertise host support, not implemented guest Vulkan functions.
 */
typedef struct venus_guest_t {
    venus_rpc_t *rpc;                  /**< Borrowed negotiated RPC, NULL when empty. */
    venus_capabilities_t capabilities; /**< Private decoded host capability snapshot. */
    uint32_t timeout_ms;               /**< Fixed 1..60000ms per-exchange deadline. */
    venus_ring_status_t lost;          /**< Sticky terminal status; RingOk while usable. */
} venus_guest_t;
/** @brief Discover host compatibility and declare the pinned guest encoder profile.
 * @param[out] guest Nonnull zero private record; unchanged on failure.
 * @param[in,out] rpc Nonnull ready unnegotiated guest RPC, borrowed until free.
 * @param[in] timeout_ms Per-exchange deadline 1..60000ms; initialization uses two.
 * @return RingOk or capability/negotiation/transport Ring status; no allocation.
 * @note Sole submission thread. Caller retains RPC cleanup on init failure.
 * This negotiates transport; it creates no native Vulkan device or WDDM adapter.
 */
venus_ring_status_t venus_guest_init(venus_guest_t *guest, venus_rpc_t *rpc, uint32_t timeout_ms);
/** @brief Submit one encoded Venus command/resource/fence request to the receiver.
 * @param[in,out] guest Nonnull initialized sole-thread frontend.
 * @param[in] request Nonnull immutable sequence-zero request, excluding negotiation
 * and capability discovery. Existing request/RPC bounds and alias rules apply.
 * @param[in] input Nullable only when length zero, private immutable input[length].
 * @param[in] length Exactly request payload_bytes, bounded by RPC scratch.
 * @param[out] response Nonnull disjoint private decoded result, zeroed on failure
 * before exchange; a completed operation error retains its validated wire result.
 * @param[out] output Nullable for no expected response payload; private output[capacity].
 * @param[in] capacity Actual output extent; failure preserves payload output.
 * @return RingOk/Again/Invalid/Limit for ordinary outcomes; terminal Ring statuses
 * close RPC and remain sticky until free/new session.
 * @note No allocation or retained payload. CPU accepted fence is not GPU completion.
 * Caller discards virtual Vulkan objects on loss and serializes application threads.
 */
venus_ring_status_t venus_guest_exchange(venus_guest_t *guest, const venus_request_t *request,
                                         const void *input, size_t length,
                                         venus_request_t *response, void *output, size_t capacity);
/** @brief Exchange using a shorter call-scoped whole-RPC deadline budget.
 * @param[in,out] guest Nonnull live sole-thread frontend; configured timeout is immutable.
 * @param[in] request Nonnull immutable sequence-zero request, excluding negotiation/capability.
 * @param[in] input Nullable only for length zero; private borrowed input[length].
 * @param[in] length Exact request payload extent, subject to existing RPC alias/bounds.
 * @param[out] response Nonnull disjoint private record, zeroed on failure as in legacy API.
 * @param[out] output Nullable only for capacity zero; private output[capacity], unchanged on failure.
 * @param[in] capacity Actual accessible response extent, subject to existing RPC alias/bounds.
 * @param[in] timeout_ms Budget1..guest->timeout_ms, at most60000, not retained or written to guest.
 * @return Existing frontend status; Invalid null/local budget before publication;
 * sticky existing loss and acknowledged terminal retirement retain legacy precedence.
 * @note No allocation/copy of live frontend/RPC, no retained payload; sole submission thread.
 * The budget covers the entire existing RPC exchange without resetting after partial ring progress.
 */
venus_ring_status_t venus_guest_exchange_timeout(venus_guest_t *guest, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity,
    uint32_t timeout_ms);
/** @brief Close borrowed RPC and reset local frontend without freeing its storage.
 * @param[in,out] guest Nullable zero/live frontend, sole thread after calls stop.
 * @note Idempotent, no allocation; caller releases channel/stream/mapping afterward.
 */
void venus_guest_free(venus_guest_t *guest);
#endif
