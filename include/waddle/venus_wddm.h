/** @file venus_wddm.h @brief Standalone userland render-only adapter model. */
#ifndef WaddleVenusWddmH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusWddmH
#include "venus_guest.h"
/** @brief Maximum negotiated guest context bindings per userland adapter. */
#define VenusWddmMaxContexts 8u
/** @brief Maximum registered device allocations per context. */
#define VenusWddmMaxAllocations 64u
/** @brief Render-only adapter capability bits; no display/scanout capability. */
typedef enum venus_wddm_flags_t {
    AdapterRender = 1, /**< Supports remote rendering command submission. */
    AdapterCompute = 2 /**< Supports remote Vulkan compute submission. */
} venus_wddm_flags_t;
/** @brief Caller-owned private adapter capabilities, immutable query output. */
typedef struct venus_wddm_capabilities_t {
    uint64_t adapter_luid;     /**< Nonzero caller-provided virtual adapter identity. */
    uint32_t version;          /**< Standalone interface ABI revision one. */
    uint32_t flags;            /**< AdapterRender|AdapterCompute; no display output. */
    uint32_t context_limit;    /**< Eight distinct frontend bindings. */
    uint32_t allocation_limit; /**< Sixty-four registered resources per context. */
} venus_wddm_capabilities_t;
/** @brief Registered resource identity; zero handle means free, no heap ownership. */
typedef struct venus_wddm_allocation_t {
    uint64_t handle;      /**< Adapter-lifetime identity, never recycled. */
    uint32_t resource_id; /**< Remote resource ID 2..65, private context namespace. */
} venus_wddm_allocation_t;
/** @brief Bound userland context; submission thread owns all fields.
 * @note Borrows frontend until destroy/discard; owns only allocation ledger identities.
 */
typedef struct venus_wddm_context_t {
    uint64_t handle;      /**< Nonzero adapter-lifetime identity, zero if free. */
    venus_guest_t *guest; /**< Borrowed negotiated frontend; never freed here. */
    uint32_t allocations; /**< Live registered allocation count. */
    venus_wddm_allocation_t slots[VenusWddmMaxAllocations]; /**< Fixed identity ledger. */
} venus_wddm_context_t;
/** @brief Caller-owned portable adapter stub; never copy an initialized record.
 * @note Sole submission thread, no allocation/native driver registration. Borrowed
 * frontends outlive their context bindings. Handles are discarded before reinit.
 */
typedef struct venus_wddm_t {
    uint64_t adapter_luid; /**< Nonzero virtual identity, zero when uninitialized. */
    uint64_t next_handle;  /**< Next nonreused ID; UINT64_MAX reserves exhaustion. */
    venus_wddm_context_t contexts[VenusWddmMaxContexts]; /**< Fixed binding ledger. */
} venus_wddm_t;
/** @brief Initialize an empty userland adapter without OS/kernel registration.
 * @param[out] adapter Nonnull zero record, unchanged on failure.
 * @param[in] adapter_luid Nonzero caller-provided virtual identity, not host address.
 * @return RingOk or RingInvalid for NULL/live/zero identity; allocation-free.
 * @note Sole submission thread; free only after all context bindings are released.
 */
venus_ring_status_t venus_wddm_init(venus_wddm_t *adapter, uint64_t adapter_luid);
/** @brief Copy immutable render/compute/no-scanout capabilities.
 * @param[in] adapter Nonnull initialized borrowed record, sole submission thread.
 * @param[out] capabilities Nonnull private output, unchanged on failure.
 * @return RingOk or RingInvalid; no allocation or ownership transfer.
 */
venus_ring_status_t venus_wddm_capabilities(const venus_wddm_t *adapter,
                                            venus_wddm_capabilities_t *capabilities);
/** @brief Bind one distinct ready negotiated guest frontend.
 * @param[in,out] adapter Nonnull initialized sole-thread record.
 * @param[in,out] guest Nonnull live negotiated frontend borrowed until destroy/discard.
 * @param[out] handle Nonnull private result, zeroed on failure.
 * @return RingOk; Invalid local/duplicate binding; Limit capacity/ID exhaustion.
 * @note No allocation/remote request or frontend ownership transfer.
 */
venus_ring_status_t venus_wddm_context_create(venus_wddm_t *adapter, venus_guest_t *guest,
                                              uint64_t *handle);
/** @brief Release a binding only after registered allocations are destroyed.
 * @param[in,out] adapter Nonnull initialized sole-thread record.
 * @param[in] context Live context handle from this adapter lifetime.
 * @return RingOk; Invalid stale/local handle; Again for live allocations.
 * @note Clears local binding only; never frees borrowed frontend/native objects.
 */
venus_ring_status_t venus_wddm_context_destroy(venus_wddm_t *adapter, uint64_t context);
/** @brief Discard all local identities after terminal frontend/RPC loss.
 * @param[in,out] adapter Nonnull initialized sole-thread record.
 * @param[in] context Live handle bound to a lost/closed frontend.
 * @return RingOk or RingInvalid for stale/local/live-session misuse.
 * @note No remote requests; caller discards old Vulkan objects, never reuses them.
 */
venus_ring_status_t venus_wddm_context_discard(venus_wddm_t *adapter, uint64_t context);
/** @brief Register existing Venus device memory with the bounded host resource ledger.
 * @param[in,out] adapter Nonnull initialized sole-thread record.
 * @param[in] context Live context handle. @param[in] blob Nonzero device-memory identity.
 * @param[in] bytes Page-aligned declared allocation bytes, 4096..one GiB.
 * @param[in] flags Valid resource Map/Share/CrossDevice policy.
 * @param[out] handle Nonnull private result, zeroed on failure.
 * @return RingOk or host Ring status; Invalid local/policy/stale handle;
 * Limit local/host quota or identity exhaustion. No publication on local failure.
 * @note No pixel allocation/map/copy; original Vulkan allocation remains caller-owned.
 */
venus_ring_status_t venus_wddm_allocation_create(venus_wddm_t *adapter, uint64_t context,
                                                 uint64_t blob, uint64_t bytes, uint32_t flags,
                                                 uint64_t *handle);
/** @brief Retire an explicit GPU fence then release registered host storage.
 * @param[in,out] adapter Nonnull initialized sole-thread record.
 * @param[in] context Live context handle. @param[in] allocation Live allocation in context.
 * @param[in] timeline Queue timeline 1..63. @param[in] fence Previously-issued nonzero identity.
 * @return RingOk or host Ring status; Invalid stale/local handles/invalid fence.
 * @note Caller destroys Vulkan references and waits for presentation release first.
 * Pending/error retains ledger. Does not destroy original Vulkan device memory.
 */
venus_ring_status_t venus_wddm_allocation_destroy(venus_wddm_t *adapter, uint64_t context,
                                                  uint64_t allocation, uint32_t timeline,
                                                  uint64_t fence);
/** @brief Release an adapter only after all bindings are released.
 * @param[in,out] adapter Nullable zero/live sole-thread record, zeroed on success.
 * @return RingOk for empty/released; Again for any live context, preserving state.
 * @note Allocation-free; borrowed frontends/native devices are never destroyed here.
 */
venus_ring_status_t venus_wddm_free(venus_wddm_t *adapter);
#endif
