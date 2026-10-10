/** @file venus_capabilities.h @brief Portable private Venus capability snapshot. */
#ifndef WaddleVenusCapabilitiesH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusCapabilitiesH
#include "venus_ring.h"
/** @brief Exact pinned public capability wire extent, no native padding. */
#define VenusCapabilitiesBytes 160u
/** @brief Pinned generated XML version, Vulkan 1.4.307. */
#define VenusPinnedXmlVersion ((1u << 22) | (4u << 12) | 307u)
/** @brief Decoded private host integers, never cast over shared/wire bytes.
 * @note Caller-owned, no pointers/allocations; immutable after decode, freely
 * copied. Pure queries are thread-safe on immutable records. Offsets in spec.
 */
typedef struct venus_capabilities_t {
    uint32_t wire_format_version; /**< Venus wire encoder version. */
    uint32_t vk_xml_version; /**< Generated registry API version. */
    uint32_t vk_ext_command_serialization_spec_version; /**< Serialization extension revision. */
    uint32_t vk_mesa_venus_protocol_spec_version; /**< Venus protocol extension revision. */
    uint32_t supports_blob_id_0; /**< Boolean CPU reply blob support. */
    uint32_t vk_extension_mask1[32]; /**< Explicit 1024-bit mask; bit zero is validity marker. */
    uint32_t allow_vk_wait_syncs; /**< Boolean upstream blocking-wait support, not host permission. */
    uint32_t supports_multiple_timelines; /**< Boolean separate queue fence support. */
    uint32_t use_guest_vram; /**< Boolean dedicated guest VRAM mode. */
} venus_capabilities_t;
/** @brief Decode exactly one private immutable capability payload in Zig.
 * @param[out] capabilities Nonnull private output, disjoint from input, zeroed on error.
 * @param[in] bytes Nonnull immutable private byte buffer[length], borrowed for call.
 * @param[in] length Exactly VenusCapabilitiesBytes, otherwise corrupt.
 * @return RingOk, RingInvalid for null, RingCorrupt for length/flag shape.
 * @note Allocation-free, no retained pointers; thread-safe on disjoint outputs.
 * Unsupported versions decode normally; compatibility is checked separately.
 */
venus_ring_status_t venus_capabilities_decode(venus_capabilities_t *capabilities,
                                              const void *bytes, size_t length);
/** @brief Check the complete pinned protocol/profile without mutation.
 * @param[in] capabilities Nullable immutable private decoded snapshot, borrowed for call.
 * @return RingOk for supported pinned profile; RingInvalid for null/shape/profile mismatch.
 * @note Pure/thread-safe, no allocation or ownership transfer; does not authorize
 * guest extensions or host policy. Exact profile is specified in IMPL_DESC.md.
 */
venus_ring_status_t venus_capabilities_compatible(const venus_capabilities_t *capabilities);
/** @brief Query an explicitly advertised protocol extension bit, in Zig.
 * @param[in] capabilities Nullable immutable private decoded snapshot.
 * @param[in] number Extension number 1..1023, validated before indexing.
 * @return One if explicitly present, zero for invalid/null/absent/legacy mask.
 * @note Pure/thread-safe, allocation-free, no retained pointers or mutation.
 */
int venus_capabilities_extension(const venus_capabilities_t *capabilities, uint32_t number);
_Static_assert(sizeof(venus_capabilities_t) == VenusCapabilitiesBytes, "Private capability C/Zig ABI");
_Static_assert(_Alignof(venus_capabilities_t) == 4, "Private capability integer alignment");
#endif
