/** @file venus_objects.h @brief Private guest Vulkan object identity/lifetime storage. */
#ifndef WaddleVenusObjectsH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusObjectsH
#include "venus_ring.h"
/** @brief Maximum borrowed record count per isolated receiver session. */
#define VenusObjectsMaxCapacity 4096u
/** @brief Initial dispatchable first word, as required by the Vulkan loader ABI. */
#define VenusObjectsLoaderMagic 0x01cdc0deu
/** @brief Private borrowed object slot, immutable except loader_data by the loader.
 * @note No allocation or shared-memory placement; storage outlives lookup results.
 * Sole owner thread; no concurrent publication. A zero id means empty.
 */
typedef struct venus_object_t {
    uintptr_t loader_data; /**< First word; loader may replace dispatchable magic. */
    uint64_t id;           /**< Monotonic nonzero host identity, never an application pointer. */
    uint64_t handle;       /**< Application address or namespace/id token. */
    uint64_t parent_id;    /**< Live parent's host identity, zero for root. */
    uint32_t kind;         /**< Nonzero external Vulkan object kind. */
    uint32_t dispatchable; /**< Exactly zero or one; selects handle representation. */
} venus_object_t;
/** @brief Caller-owned sole-thread registry, never copy or mutate live fields.
 * @note Borrows private storage/context until free; no allocation or transport calls.
 */
typedef struct venus_objects_t {
    venus_object_t *slots; /**< Exclusive private storage[capacity], NULL when empty. */
    void *context;         /**< Nonnull borrowed frontend, outlives registry. */
    uint32_t capacity;     /**< Snapshotted actual record count, 1..4096. */
    uint32_t live_count;   /**< Current occupied record count. */
    uint32_t next_id;      /**< Next host identity, never wraps or reuses. */
    uint32_t namespace_id; /**< Nonzero process-unique session namespace, never reused. */
} venus_objects_t;
/** @brief Initialize exclusive borrowed slots with a unique session namespace.
 * @param[out] objects Nonnull zero private owner, unchanged on failure.
 * @param[in,out] slots Nonnull private actual slots[capacity], cleared on success;
 * disjoint from owner, frontend scratch and shared memory. Borrowed until free.
 * @param[in] capacity Actual accessible records, 1..4096.
 * @param[in] namespace_id Nonzero process-unique namespace, never reused by caller.
 * @param[in,out] context Nonnull borrowed frontend, not dereferenced here.
 * @return RingOk or RingInvalid for null/bounds/overlap/overflow/live owner.
 * @note Sole thread; no allocation. Caller owns namespace generation/overflow checks.
 */
venus_ring_status_t venus_objects_init(venus_objects_t *objects, venus_object_t *slots,
                                       uint32_t capacity, uint32_t namespace_id, void *context);
/** @brief Reserve a private object before serializing its host creation.
 * @param[in,out] objects Nonnull live sole-thread owner.
 * @param[in] kind Nonzero external Vulkan object kind, preserved without translation.
 * @param[in] parent_id Zero root or live parent host identity in this registry.
 * @param[in] dispatchable Exactly0/1, selects token/address handle.
 * @param[out] object Nonnull disjoint private output, NULL on failure; borrowed
 * immutable live slot until release/free, except loader header replacement.
 * @return RingOk, Invalid for local/parent errors, Limit for full/exhausted storage,
 * Corrupt for contradictory private storage/count (abandon old receiver session).
 * @note No allocation. Failed host creation must release this reservation; IDs never reuse.
 */
venus_ring_status_t venus_objects_reserve(venus_objects_t *objects, uint32_t kind,
                                          uint64_t parent_id, uint32_t dispatchable,
                                          venus_object_t **object);
/** @brief Validate an application handle without dereferencing that handle.
 * @param[in] objects Nonnull live sole-thread owner, borrowed for call.
 * @param[in] handle Nonzero application token or record address.
 * @param[in] kind Exact nonzero expected Vulkan object kind.
 * @param[in] dispatchable Exactly0/1 expected representation.
 * @param[out] object Nonnull disjoint private output, NULL on failure; borrowed
 * matching live slot until release/free. Caller may only allow loader_data replacement.
 * @return RingOk or RingInvalid; no allocation or transport calls.
 */
venus_ring_status_t venus_objects_lookup(const venus_objects_t *objects, uint64_t handle,
                                         uint32_t kind, uint32_t dispatchable,
                                         venus_object_t **object);
/** @brief Translate a validated host reply identity to an existing private slot.
 * @param[in] objects Nonnull live sole-thread owner, borrowed for call.
 * @param[in] id Nonzero host ID, must already exist in this session.
 * @param[in] kind Exact nonzero expected Vulkan object kind.
 * @param[out] object Nonnull disjoint private output, NULL on failure; borrowed
 * existing slot until release/free. Never creates objects from peer data.
 * @return RingOk or RingInvalid; no allocation, bounded scan.
 */
venus_ring_status_t venus_objects_lookup_id(const venus_objects_t *objects, uint64_t id,
                                            uint32_t kind, venus_object_t **object);
/** @brief Retire an application handle after host destruction or creation rollback.
 * @param[in,out] objects Nonnull live sole-thread owner.
 * @param[in] handle Nonzero application token/address.
 * @param[in] kind Exact nonzero expected object kind.
 * @param[in] dispatchable Exactly0/1 expected representation.
 * @return RingOk and cleared slot; Again while any live child names this object;
 * Invalid for foreign/retired/wrong-kind/local arguments. No transport cancellation.
 * @note Caller serializes host destruction before local release; no allocation.
 */
venus_ring_status_t venus_objects_release(venus_objects_t *objects, uint64_t handle, uint32_t kind,
                                          uint32_t dispatchable);
/** @brief Clear borrowed registry after host objects/session are retired.
 * @param[in,out] objects Nullable zero/live owner, sole thread after calls stop.
 * @note No transport calls or storage frees. Caller first destroys host objects or
 * abandons old receiver; all borrowed slot results expire. Idempotent.
 */
void venus_objects_free(venus_objects_t *objects);
_Static_assert(sizeof(uintptr_t) == 8, "Vulkan guest ABI requires x86_64");
_Static_assert(sizeof(venus_object_t) == 40, "Vulkan object slot ABI");
_Static_assert(offsetof(venus_object_t, loader_data) == 0, "Loader first word");
_Static_assert(sizeof(venus_objects_t) == 32, "Vulkan registry ABI");
#endif
