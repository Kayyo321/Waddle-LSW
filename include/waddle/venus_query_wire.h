/** @file venus_query_wire.h @brief Bounded core physical enumeration/query
 * transactions. */
#ifndef WaddleVenusQueryWireH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusQueryWireH
#include "venus_instance_wire.h"
/** @brief Maximum physical devices represented per instance; no storage. */
#define VenusQueryMaxDevices 16u
/** @brief Encode count/fill enumeration with preassigned physical identities.
 * @param[in] instance_id Nonzero live host instance identity.
 * @param[in] ids NULL iff count is zero; otherwise borrowed unique ids[count],
 * nonzero, distinct from instance_id; caller retains reservations until
 * reply/abandonment.
 * @param[in] count Reserved ID count, 0..16; zero selects count-only query.
 * @param[out] bytes Nonnull private output[capacity], disjoint from
 * ids/written.
 * @param[in] capacity Actual accessible output extent, 8..16MiB.
 * @param[out] written Nonnull private prefix length, disjoint from
 * input/output.
 * @return RingOk; Invalid null/alias/overflow/identity/count; Limit short
 * capacity.
 * @note Allocation-free; output/prefix preserved on failure; concurrent
 * disjoint calls safe.
 */
venus_ring_status_t venus_query_wire_enumerate(uint64_t instance_id,
                                               const uint64_t *ids,
                                               uint32_t count, void *bytes,
                                               size_t capacity,
                                               size_t *written);
/** @brief Validate an enumeration reply against count mode or reserved fill
 * identities.
 * @param[out] result Nonnull private VkResult; unchanged on malformed reply.
 * @param[out] count Nonnull private returned count; unchanged on malformed
 * reply.
 * @param[in] ids NULL iff capacity is zero; otherwise immutable reserved
 * ids[capacity].
 * @param[in] capacity Reserved count, 0..16; zero selects count-only query.
 * @param[in] bytes Nonnull immutable private reply[length], borrowed during
 * call.
 * @param[in] length Actual accessible reply extent, 0..16MiB; trailing capacity
 * ignored.
 * @return RingOk validated success/incomplete/negative VkResult; Invalid local
 * arguments/alias/overflow; Corrupt
 * malformed/truncated/count/tag/result/identity.
 * @note All inputs/outputs disjoint; no wire pointer dereference/allocation;
 * thread-safe.
 */
venus_ring_status_t
venus_query_wire_enumerate_reply(venus_vk_result_t *result, uint32_t *count,
                                 const uint64_t *ids, uint32_t capacity,
                                 const void *bytes, size_t length);
/** @brief Encode a core fixed-output query with exact pinned partial array
 * tags.
 * @param[in] command Exact pinned ID 3(features), 6(properties) or 8(memory).
 * @param[in] physical_id Nonzero live host physical identity, never a pointer.
 * @param[out] bytes Nonnull private output[capacity], disjoint from written.
 * @param[in] capacity Actual accessible output extent, 8..16MiB.
 * @param[out] written Nonnull private initialized prefix length, unchanged on
 * failure.
 * @return RingOk; Invalid local/null/alias/overflow/command/identity; Limit
 * short capacity.
 * @note Allocation-free, output preserved on failure; concurrent disjoint calls
 * safe.
 */
venus_ring_status_t venus_query_wire_fixed(uint32_t command,
                                           uint64_t physical_id, void *bytes,
                                           size_t capacity, size_t *written);
#endif
