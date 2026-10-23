/** @file m2_owner_ledger.h @brief Fixed qualification-only exact native owner ledger.
 * @note One serialized caller owns each ledger: the ICD lock or sole fixture UI
 * thread. No allocations, locks, native calls or borrowed-handle retirement here.
 */
#ifndef WaddleM2OwnerLedgerH
/** @brief Include guard; no storage. */
#define WaddleM2OwnerLedgerH
#include <stddef.h>
#include <stdint.h>
/** @brief Maximum simultaneous acquisitions, including reserved slots. */
#define M2OwnerCapacity 32u
/** @brief Exact snapshot ABI version; incompatible inputs fail without writes. */
#define M2OwnerVersion 1u
/** @brief Native resource classes; each value has its own exact identity domain. */
typedef enum m2_owner_kind_t {
    M2OwnerDc = 1, /**< GetDC owner, retired by matching ReleaseDC. */
    M2OwnerWindow = 2 /**< CreateWindow owner, retired by DestroyWindow. */
} m2_owner_kind_t;
/** @brief Sticky observation failures; none authorizes forgetting an owner. */
typedef enum m2_owner_failure_t {
    M2OwnerInvalid = 1, /**< Invalid ABI/identity operation. */
    M2OwnerLimit = 2, /**< Full ledger or exhausted monotonic counter. */
    M2OwnerDuplicate = 4, /**< Ambiguous concurrent native identity. */
    M2OwnerReleaseFailed = 8, /**< Actual/injected native release failed. */
    M2OwnerInconsistent = 16 /**< Read-only scan contradicts recorded counters. */
} m2_owner_failure_t;
/** @brief One reserved/acquired native owner, retained until successful release. */
typedef struct m2_owner_entry_t {
    uint64_t id; /**< Monotonic acquisition identity; zero means empty. */
    uintptr_t parent; /**< Borrowed HWND for DC; zero for owned HWND. */
    uintptr_t value; /**< Owned API acquisition bits; zero while reserved. */
    uint32_t kind; /**< m2_owner_kind_t; zero for empty. */
} m2_owner_entry_t;
/** @brief Caller-owned fixed ledger. Initialize as {.next_id=1}; never copy live. */
typedef struct m2_owner_ledger_t {
    uint64_t next_id; /**< Next reservation ID, never wraps/reuses. */
    uint64_t acquired; /**< Successful native acquisitions, excludes failed attempts. */
    uint64_t retired; /**< Successful exact native retirements. */
    uint32_t failures; /**< Sticky m2_owner_failure_t bitset; blocks acquisition. */
    m2_owner_entry_t entries[M2OwnerCapacity]; /**< Exclusive fixed identities. */
} m2_owner_ledger_t;
/** @brief Fixed-width read-only snapshot; no pointer or ownership transfer. */
typedef struct m2_owner_snapshot_t {
    uint32_t version; /**< Exactly M2OwnerVersion. */
    uint32_t bytes; /**< Exactly sizeof(m2_owner_snapshot_t). */
    uint64_t next_id; /**< Monotonic reservation high-water mark. */
    uint64_t acquired; /**< Successful acquisition count. */
    uint64_t retired; /**< Successful release count. */
    uint64_t first_live_id; /**< First retained ID, zero when no retained owner. */
    uint64_t first_live_parent; /**< Matching borrowed parent bits. */
    uint64_t first_live_value; /**< Matching owned native bits, never closed by query. */
    uint32_t live; /**< Acquired identities, counted from actual entries. */
    uint32_t pending; /**< Reserved slots awaiting API result. */
    uint32_t failures; /**< Sticky failures plus observed state inconsistencies. */
    uint32_t reserved; /**< Must be zero; fixed future ABI space. */
} m2_owner_snapshot_t;
_Static_assert(sizeof(m2_owner_snapshot_t) == 72, "M2 owner snapshot ABI");
/** @brief Reserve identity BEFORE native acquisition, rejecting unsafe attempts.
 * @param[in,out] ledger Nonnull exclusive initialized owner; no pointer retained.
 * @param[in] kind DC or HWND. @param[in] parent Nonzero borrowed HWND for DC,
 * zero for HWND. @return Nonzero reserved ID; zero invalid/full/faulted/exhausted.
 * @note Serialized caller; zero return means the caller MUST NOT acquire natively.
 */
uint64_t m2_owner_reserve(m2_owner_ledger_t *ledger, uint32_t kind, uintptr_t parent);
/** @brief Record the actual result in its already-reserved identity slot.
 * @param[in,out] ledger Nonnull exclusive owner. @param[in] id Exact reserved ID.
 * @param[in] value Actual native result; zero cancels a failed acquisition.
 * @return One recorded, zero invalid/duplicate. A duplicate successful acquisition
 * remains recorded and MUST be released by its exact ID, even on zero return.
 * @note Serialized; never calls the OS or discards a successful acquisition.
 */
int m2_owner_acquired(m2_owner_ledger_t *ledger, uint64_t id, uintptr_t value);
/** @brief Resolve the sole acquired identity before attempting a native release.
 * @param[in,out] ledger Nonnull exclusive owner. @param[in] kind Resource class.
 * @param[in] parent Borrowed parent bits. @param[in] value Owned native bits.
 * @return Exact ID or zero and sticky failure for absent/ambiguous identity.
 * @note Serialized; zero return forbids releasing that unproven native owner.
 */
uint64_t m2_owner_lookup(m2_owner_ledger_t *ledger, uint32_t kind, uintptr_t parent, uintptr_t value);
/** @brief Record actual native release result for an exact acquired identity.
 * @param[in,out] ledger Nonnull exclusive owner. @param[in] id Exact live ID.
 * @param[in] succeeded Actual API result, zero failure/nonzero success.
 * @return One retired, zero invalid/failed; failed release retains the entry.
 * @note Serialized; cleanup remains permitted while acquisition is fault-blocked.
 */
int m2_owner_retired(m2_owner_ledger_t *ledger, uint64_t id, int succeeded);
/** @brief Copy validated state without changing it or calling native APIs.
 * @param[in] ledger Nonnull borrowed ledger, externally serialized.
 * @param[in] version M2OwnerVersion. @param[out] output Nonnull distinct exclusive
 * snapshot, unchanged on bad ABI. @param[in] bytes Exact output size.
 * @return One snapshot copied, zero bad pointers/ABI. Live/error data still copies.
 */
int m2_owner_snapshot(const m2_owner_ledger_t *ledger, uint32_t version, m2_owner_snapshot_t *output, size_t bytes);
/** @brief Evaluate exact empty ownership, independently of process totals.
 * @param[in] snapshot Nullable borrowed copied snapshot; retained nowhere.
 * @return One only for valid ABI, zero faults/pending/live and balanced identities.
 * @note Pure/thread-safe; no native access or state mutation.
 */
int m2_owner_clean(const m2_owner_snapshot_t *snapshot);
#endif
