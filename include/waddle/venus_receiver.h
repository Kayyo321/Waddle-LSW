/** @file venus_receiver.h @brief Public-ABI Venus bootstrap and CPU reply owner. */
#ifndef WaddleVenusReceiverH
/** @brief Compile-time include guard; no ownership or storage. */
#define WaddleVenusReceiverH
#include "venus_ring.h"
/** @brief Pinned upstream version-zero Venus capability set bytes. */
#define VenusCapabilityBytes 160u
/** @brief Opaque Linux receiver; created/destroyed by one session thread.
 * @note Owns renderer singleton, context, reply resource/map, command scratch,
 * and callback lifetime. No borrowed IVSHMEM or socket ownership. Never copy.
 */
typedef struct venus_receiver_t venus_receiver_t;
/** @brief Create one renderer bootstrap context and CPU reply stream.
 * @param[out] receiver Nonnull pointer to initially-null owner pointer; success
 * transfers ownership, released with venus_receiver_destroy; failure stays null.
 * @param[in] command_capacity Power-of-two private bytes, 64..16777216.
 * @param[in] reply_bytes Power-of-two CPU reply extent, 4096..16777216.
 * @return RingOk; RingInvalid for local arguments; RingAgain if renderer claimed;
 * RingCorrupt for allocation, renderer, capability, context, blob or map failure.
 * @note Linux/session-thread-only; no competing global renderer API calls.
 * Trusted server executable must already be installed or configured through
 * RENDER_SERVER_EXEC_PATH. CPU bootstrap does not select a hardware GPU.
 */
venus_ring_status_t venus_receiver_create(venus_receiver_t **receiver, uint32_t command_capacity,
                                          uint64_t reply_bytes);
/** @brief Release a partial/live owner and null its pointer.
 * @param[in,out] receiver Nullable pointer to nullable owned record.
 * @note Session-thread-only, no concurrent API calls. Context/callbacks stop
 * before resource unmap, renderer cleanup, scratch free and singleton release.
 * Safe with pending fences; no deadline waits or transferred handles remain.
 */
void venus_receiver_destroy(venus_receiver_t **receiver);
/** @brief Copy the retained public Venus capability set for negotiation.
 * @param[in] receiver Nonnull borrowed live owner.
 * @param[out] output Nonnull disjoint private buffer[length], unchanged on error.
 * @param[in] length Exactly VenusCapabilityBytes.
 * @return RingOk or RingInvalid; no allocation or ownership transfer.
 * @note Session-thread-only; bytes are the pinned upstream little-endian capset.
 */
venus_ring_status_t venus_receiver_capabilities(const venus_receiver_t *receiver, void *output,
                                                size_t length);
/** @brief Copy a bounded complete command bundle into private scratch and submit.
 * @param[in,out] receiver Nonnull live session-thread-owned record.
 * @param[in] commands Nonnull immutable private or shared input[length], disjoint
 * from owner; remains stable during copy. Renderer sees only the private copy.
 * @param[in] length Multiple of four, 8..configured command_capacity bytes.
 * @param[out] fence Nonnull disjoint private output, zeroed on failure; successful
 * monotonically increasing nonzero CPU fence, not a GPU completion guarantee.
 * @return RingOk, RingInvalid for local bounds, RingAgain for in-flight bundle,
 * RingCorrupt for poisoned owner/public submit/fence failure or exhausted IDs.
 * @note Session-thread-only, no allocations. Copy replies before next submission.
 * Venus semantics/handles decoded by upstream; own envelope bounds checked in Zig.
 */
venus_ring_status_t venus_receiver_submit(venus_receiver_t *receiver, const void *commands,
                                          size_t length, uint64_t *fence);
/** @brief Acquire completion of the most recent timeline-zero submission.
 * @param[in] receiver Nonnull borrowed live owner.
 * @return RingOk if no pending CPU work; RingAgain if fence not retired;
 * RingInvalid if null, RingCorrupt if poisoned or callback identity failed.
 * @note Session-thread-only, nonblocking. Runtime must enforce deadline and
 * cancellation; CPU completion does not establish GPU queue completion.
 */
venus_ring_status_t venus_receiver_poll(const venus_receiver_t *receiver);
/** @brief Copy a completed CPU reply range into private caller storage.
 * @param[in] receiver Nonnull borrowed live owner; no concurrent submit/destroy.
 * @param[in] offset Byte offset within mapped reply extent.
 * @param[out] output Nonnull disjoint private output[length], unchanged on error.
 * @param[in] length Nonzero bounded reply bytes; caller decodes Venus reply size.
 * @return RingOk, RingAgain while CPU work pending, RingInvalid for bounds/null,
 * or RingCorrupt for poisoned owner; no allocation or ownership transfer.
 * @note Session-thread-only; copy needed replies before reusing command stream.
 */
venus_ring_status_t venus_receiver_reply(const venus_receiver_t *receiver, uint64_t offset,
                                         void *output, size_t length);

/** @brief Maximum additional registered resource count; fixed owner ledger. */
#define VenusReceiverMaxResources 64u
/** @brief Default additional registered-storage quota, separate from bootstrap. */
#define VenusReceiverDefaultResourceBytes 67108864u
/** @brief Resource storage flags; validated locally before SDK translation. */
typedef enum venus_resource_flags_t {
    ResourceMap = 1,        /**< CPU mapping allowed; required alone for blob zero SHM. */
    ResourceShare = 2,      /**< Device storage may be shared externally. */
    ResourceCrossDevice = 4 /**< Requires Share; device export must support DMA-BUF. */
} venus_resource_flags_t;
/** @brief Configure additional registry limits while empty and CPU-quiescent.
 * @param[in,out] receiver Nonnull session-thread-owned live record.
 * @param[in] count Maximum additional registered entries, 1..64.
 * @param[in] bytes Maximum declared additional storage, 4096..one GiB.
 * @return RingOk, RingInvalid for ranges/nonempty registry, or poll failure.
 * @note No allocation; defaults 64/64MiB. Counts registered storage, not arbitrary
 * Vulkan VRAM allocations. Bootstrap reply/scratch limits remain independent.
 */
venus_ring_status_t venus_receiver_resource_limits(venus_receiver_t *receiver, uint32_t count,
                                                   uint64_t bytes);
/** @brief Acquire bounded additional storage via the public Venus context ABI.
 * @param[in,out] receiver Nonnull session-thread-owned record, CPU-quiescent.
 * @param[in] resource_id Unique ledger ID, 2..65; zero/one reserved.
 * @param[in] blob_id Zero for CPU SHM, otherwise an existing Venus device-memory ID.
 * @param[in] bytes Nonzero page-aligned declared storage, at most one GiB.
 * @param[in] flags ResourceMap/Share/CrossDevice only; CrossDevice requires Share;
 * blob zero requires exactly ResourceMap.
 * @return RingOk; RingInvalid for bounds/duplicate; RingLimit for quota; poll
 * status for pending/poisoned owner; RingCorrupt on SDK failure (poisons owner).
 * @note No per-resource Waddle allocation; ownership enters ledger only on success.
 * Resource remains owned until explicit free or receiver destroy. GPU allocation
 * must already exist for nonzero blob; no hard GPU VRAM isolation is implied.
 */
venus_ring_status_t venus_receiver_resource_create(venus_receiver_t *receiver, uint32_t resource_id,
                                                   uint64_t blob_id, uint64_t bytes,
                                                   uint32_t flags);
/** @brief Release registered storage after all Venus references are destroyed.
 * @param[in,out] receiver Nonnull CPU-quiescent session-thread-owned record.
 * @param[in] resource_id Live registered ID, 2..65; bootstrap cannot be freed here.
 * @return RingOk, RingInvalid for ID/not live, poll status, or RingCorrupt on
 * unmap failure (poisons owner; cleanup accounting retained for destruction).
 * @note Unmaps/unrefs, refunds declared quota and zeros entry. Caller must destroy
 * any Venus ring/object referencing it first; no GPU queue completion is inferred.
 */
venus_ring_status_t venus_receiver_resource_free(venus_receiver_t *receiver, uint32_t resource_id);
/** @brief Read mapped resource storage into private caller output after CPU completion.
 * @param[in,out] receiver Nonnull live session-thread-owned record; lazy map owned.
 * @param[in] resource_id Live ResourceMap ID, 2..65; CPU SHM or host-coherent device memory.
 * @param[in] offset Byte offset within declared extent.
 * @param[out] output Nonnull disjoint private output[length], unchanged on failure.
 * @param[in] length Nonzero bytes, must fit extent after offset.
 * @return RingOk, RingInvalid, poll status, or RingCorrupt for poisoned SDK map.
 * @note Zig validates bounds before mapping/copy; no native pointer escapes.
 * Caller must retire GPU use and ensure device memory is host coherent.
 */
venus_ring_status_t venus_receiver_resource_read(venus_receiver_t *receiver, uint32_t resource_id,
                                                 uint64_t offset, void *output, size_t length);
/** @brief Write private caller input into mapped resource storage after CPU completion.
 * @param[in,out] receiver Nonnull live session-thread-owned record; lazy map owned.
 * @param[in] resource_id Live ResourceMap ID, 2..65; CPU SHM or host-coherent device memory.
 * @param[in] offset Byte offset within declared extent.
 * @param[in] input Nonnull immutable disjoint private input[length].
 * @param[in] length Nonzero bytes, must fit extent after offset.
 * @return RingOk, RingInvalid, poll status, or RingCorrupt for poisoned SDK map.
 * @note No native pointer escapes; borrowed input retained only for this call.
 * Caller must retire GPU use and ensure device memory is host coherent.
 */
venus_ring_status_t venus_receiver_resource_write(venus_receiver_t *receiver, uint32_t resource_id,
                                                  uint64_t offset, const void *input,
                                                  size_t length);
/** @brief Pinned renderer timeline slots; zero CPU, 1..63 GPU queues. */
#define VenusReceiverTimelineCount 64u
/** @brief Fence previously associated Venus GPU queue after CPU dispatch completes.
 * @param[in,out] receiver Nonnull live session-thread-owned record.
 * @param[in] timeline Queue's existing Venus timeline, 1..63, never CPU zero.
 * @param[out] fence Nonnull disjoint private output, zeroed on failure; success
 * transfers a nonzero per-queue identity, not memory/handle ownership.
 * @return RingOk; RingInvalid for null/bad timeline; RingAgain for CPU work or
 * a pending fence on this queue; RingCorrupt for poison/exhaustion/SDK failure.
 * @note No allocation. Queue association must already exist through Venus;
 * SDK rejection poisons owner. Other queues can have independent pending fences.
 */
venus_ring_status_t venus_receiver_gpu_fence(venus_receiver_t *receiver, uint32_t timeline,
                                             uint64_t *fence);
/** @brief Acquire retirement of an explicit previously-issued GPU queue fence.
 * @param[in] receiver Nonnull borrowed live owner, sole session thread.
 * @param[in] timeline Queue timeline, 1..63.
 * @param[in] fence Nonzero previously-issued per-queue identity.
 * @return RingOk retired, RingAgain pending, RingInvalid null/bounds/unissued,
 * RingCorrupt poisoned. No allocation or ownership transfer.
 * @note May poll while later CPU work is pending; CPU retirement does not imply
 * GPU retirement. Upstream callback gives no device-loss status: consult Vulkan
 * replies/health before claiming execution success. Caller enforces deadline.
 */
venus_ring_status_t venus_receiver_gpu_poll(const venus_receiver_t *receiver, uint32_t timeline,
                                            uint64_t fence);
/** @brief Default host CPU/GPU fence retirement budget in milliseconds. */
#define VenusReceiverDefaultTimeoutMs 5000u
/** @brief Configure trusted host fence budgets only while all work is quiescent.
 * @param[in,out] receiver Nonnull live session-thread-owned record.
 * @param[in] cpu_ms CPU decoder budget, 1..60000ms.
 * @param[in] gpu_ms Per-GPU-queue budget, 1..60000ms.
 * @return RingOk, RingInvalid for ranges/null, RingAgain for pending CPU/GPU,
 * RingCorrupt for poison; no allocation or ownership transfer.
 * @note Guests cannot change policy; this does not reset pending deadlines.
 */
venus_ring_status_t venus_receiver_timeouts(venus_receiver_t *receiver, uint32_t cpu_ms,
                                            uint32_t gpu_ms);
/** @brief Check independent absolute fence deadlines and cancellation.
 * @param[in,out] receiver Nonnull live session-thread-owned record.
 * @param[in] cancel Nullable borrowed atomic cancellation flag, acquire-read,
 * retained only for call. Nonzero poisons owner and returns Cancelled.
 * @return RingOk healthy (not completion), RingInvalid null, RingCorrupt poisoned
 * or clock error, RingTimeout newly expired pending fence, RingCancelled cancel.
 * @note Allocation-free. Failure requires destruction/new session; cannot preempt
 * blocked SDK/kernel calls. Runtime must sample during framing/waits.
 */
venus_ring_status_t venus_receiver_health(venus_receiver_t *receiver,
                                          const _Atomic uint32_t *cancel);
/** @brief Export completed shareable Venus device memory as a DMA-BUF.
 * @param[in,out] receiver Nonnull live session-thread-owned receiver.
 * @param[in] resource_id Live nonzero-blob resource ID 2..65 with ResourceShare.
 * @param[in] timeline Existing GPU queue timeline 1..63, ordering image writes.
 * @param[in] fence Previously-issued nonzero fence ordering all allocation writes.
 * @param[out] fd Nonnull private output; set -1 on failure. Success transfers one
 * owned CLOEXEC DMA-BUF FD; caller closes after handoff/cancellation.
 * @return RingOk; RingInvalid for arguments/resource/policy/unissued fence;
 * RingAgain for pending CPU/GPU; RingCorrupt for poison/SDK/type/CLOEXEC failure.
 * @note Linux/session-thread-only. No pixel mapping/copy or ledger mutation.
 * Caller checks Vulkan results and prevents writes/reuse until compositor release.
 * Export failure does not poison receiver. FD lifetime is independent of ledger;
 * resource/image ownership and quota remain until explicit release/destroy.
 */
venus_ring_status_t venus_receiver_resource_export(venus_receiver_t *receiver, uint32_t resource_id,
                                                   uint32_t timeline, uint64_t fence, int *fd);
#endif
