#ifndef WaddleInteropLayoutH
#define WaddleInteropLayoutH
/**
 * @file interop_layout.h
 * @brief Allocation-free C ABI over the frozen layout-v3 pure Zig validators.
 *
 * These calls parse caller-owned private snapshots and calculate mapping-relative
 * bounds. They never dereference a live mapping, perform atomics, retain pointers,
 * allocate memory, or own mappings. Callers must acquire descriptor Ready and
 * copy its bytes before validation. Distinct call state is thread-safe; each
 * output object must be exclusively owned for the duration of its call.
 */
#include <stddef.h>
#include <stdint.h>

/** @brief Result of one layout-v3 ABI call. */
typedef enum interop_layout_status_t {
    InteropLayoutStatusOk = 0, /**< Operation completed; output contains the result. */
    InteropLayoutStatusInvalidArgument = 1, /**< Null, wrong-sized, unaligned, overflowing, or aliased call arguments. */
    InteropLayoutStatusInvalidLayout = 2 /**< Snapshot or mapping facts violate frozen layout v3. */
} interop_layout_status_t;

/** @brief Validated immutable header facts; caller-owned value, no mapping ownership. */
typedef struct interop_shm_header_snapshot_t {
    uint64_t session_epoch; /**< Nonzero epoch matching the v2 Hello. */
    uint64_t total_bytes; /**< Validated mapping length. */
    uint32_t slot_count; /**< Validated descriptor count. */
    uint32_t audio_ring_count; /**< Validated audio ring count. */
    uint64_t heap_offset; /**< Validated mapping-relative heap start. */
    uint64_t heap_bytes; /**< Validated heap capacity. */
    uint32_t page_bytes; /**< Validated allocation page size. */
    uint64_t guest_heartbeat; /**< Value copied from the private snapshot. */
    uint64_t host_heartbeat; /**< Value copied from the private snapshot. */
} interop_shm_header_snapshot_t;

/** @brief Validated ready-frame metadata; pixel range remains mapping-relative. */
typedef struct interop_slot_snapshot_t {
    uint32_t descriptor_index; /**< Validated descriptor-table index. */
    uint64_t window; /**< Validated nonzero HWND identity. */
    uint64_t generation; /**< Validated nonzero window incarnation. */
    uint64_t frame_sequence; /**< Validated nonzero frame sequence. */
    uint64_t data_offset; /**< Validated mapping-relative pixel start. */
    uint32_t data_bytes; /**< Validated page-multiple pixel capacity. */
    uint32_t width; /**< Validated physical pixel width. */
    uint32_t height; /**< Validated physical pixel height. */
    uint32_t stride; /**< Validated bytes per row. */
} interop_slot_snapshot_t;

/** @brief Clamped BGRA geometry and exact rounded allocation size. */
typedef struct interop_frame_layout_t {
    uint32_t width; /**< Clamped physical pixel width. */
    uint32_t height; /**< Clamped physical pixel height. */
    uint32_t stride; /**< 64-byte-aligned row size. */
    uint64_t pixel_bytes; /**< Exact stride times height payload bytes. */
    uint32_t data_bytes; /**< Page-rounded allocation capacity. */
} interop_frame_layout_t;

/** @brief Mapping-relative bounds of one fixed-format audio ring. */
typedef struct interop_audio_ring_bounds_t {
    uint32_t ring_index; /**< Validated index in [0, 8). */
    uint64_t header_offset; /**< Mapping-relative ring header offset. */
    uint64_t pcm_offset; /**< Mapping-relative PCM start. */
    uint32_t pcm_bytes; /**< PCM capacity in bytes. */
    uint32_t capacity_frames; /**< PCM capacity in stereo frames. */
} interop_audio_ring_bounds_t;

_Static_assert(sizeof(interop_layout_status_t) == 4, "layout status ABI");
_Static_assert(sizeof(interop_shm_header_snapshot_t) == 64, "header snapshot ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, session_epoch) == 0, "header snapshot epoch ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, total_bytes) == 8, "header snapshot length ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, slot_count) == 16, "header snapshot slots ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, audio_ring_count) == 20, "header snapshot audio ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, heap_offset) == 24, "header snapshot heap ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, heap_bytes) == 32, "header snapshot heap length ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, page_bytes) == 40, "header snapshot page ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, guest_heartbeat) == 48, "header snapshot guest heartbeat ABI");
_Static_assert(offsetof(interop_shm_header_snapshot_t, host_heartbeat) == 56, "header snapshot host heartbeat ABI");
_Static_assert(sizeof(interop_slot_snapshot_t) == 56, "slot snapshot ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, descriptor_index) == 0, "slot snapshot index ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, window) == 8, "slot snapshot window ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, generation) == 16, "slot snapshot generation ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, frame_sequence) == 24, "slot snapshot sequence ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, data_offset) == 32, "slot snapshot offset ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, data_bytes) == 40, "slot snapshot capacity ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, width) == 44, "slot snapshot width ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, height) == 48, "slot snapshot height ABI");
_Static_assert(offsetof(interop_slot_snapshot_t, stride) == 52, "slot snapshot stride ABI");
_Static_assert(sizeof(interop_frame_layout_t) == 32, "frame layout ABI");
_Static_assert(offsetof(interop_frame_layout_t, width) == 0, "frame layout width ABI");
_Static_assert(offsetof(interop_frame_layout_t, height) == 4, "frame layout height ABI");
_Static_assert(offsetof(interop_frame_layout_t, stride) == 8, "frame layout stride ABI");
_Static_assert(offsetof(interop_frame_layout_t, pixel_bytes) == 16, "frame layout pixels ABI");
_Static_assert(offsetof(interop_frame_layout_t, data_bytes) == 24, "frame layout capacity ABI");
_Static_assert(sizeof(interop_audio_ring_bounds_t) == 32, "audio bounds ABI");
_Static_assert(offsetof(interop_audio_ring_bounds_t, ring_index) == 0, "audio bounds index ABI");
_Static_assert(offsetof(interop_audio_ring_bounds_t, header_offset) == 8, "audio bounds header ABI");
_Static_assert(offsetof(interop_audio_ring_bounds_t, pcm_offset) == 16, "audio bounds PCM ABI");
_Static_assert(offsetof(interop_audio_ring_bounds_t, pcm_bytes) == 24, "audio bounds bytes ABI");
_Static_assert(offsetof(interop_audio_ring_bounds_t, capacity_frames) == 28, "audio bounds frames ABI");

/**
 * @brief Validate one private 4096-byte header snapshot against actual mapping facts.
 * @param[in] header_bytes Non-NULL readable private bytes; must not alias live mapping atomics.
 * @param[in] header_length Exactly 4096, otherwise InvalidArgument.
 * @param[in] mapping_address Actual non-NULL borrowed mapping base address; never dereferenced.
 * @param[in] mapping_length Actual mapped byte length, expected to be 2 GiB.
 * @param[in] expected_session_epoch Nonzero epoch negotiated by Hello.
 * @param[out] snapshot Non-NULL aligned caller-owned output, disjoint from header_bytes.
 * @return Ok on success, InvalidArgument for invalid pointers/length/alias/overflow,
 *         or InvalidLayout when snapshot or mapping facts violate layout v3.
 * @note Allocation-free, does not retain pointers or perform atomic access. No shared
 *       mutable state; output is unchanged on failure. Caller owns all memory and
 *       serializes access to snapshot. Concurrent calls are safe for disjoint outputs.
 */
interop_layout_status_t interop_layout_validate_shm_header(
    const uint8_t *header_bytes,
    size_t header_length,
    uintptr_t mapping_address,
    uint64_t mapping_length,
    uint64_t expected_session_epoch,
    interop_shm_header_snapshot_t *snapshot);

/**
 * @brief Validate one private Ready descriptor snapshot against a validated header and identity.
 * @param[in] descriptor_bytes Non-NULL readable private bytes, not live atomic storage.
 * @param[in] descriptor_length Exactly 64, otherwise InvalidArgument.
 * @param[in] header Non-NULL aligned validated header value; disjoint from output.
 * @param[in] descriptor_index Index in the fixed descriptor table.
 * @param[in] expected_window Nonzero expected HWND identity.
 * @param[in] expected_generation Nonzero expected window generation.
 * @param[out] snapshot Non-NULL aligned caller-owned output, disjoint from both inputs.
 * @return Ok on success, InvalidArgument for invalid pointers/length/alias/overflow,
 *         or InvalidLayout when descriptor contents violate layout v3.
 * @note Allocation-free; caller performs acquire ownership transition and creates the
 *       private snapshot. It never reads mapped memory or atomics. Output is unchanged
 *       on failure; caller owns inputs and output and serializes output access.
 */
interop_layout_status_t interop_layout_validate_slot_descriptor(
    const uint8_t *descriptor_bytes,
    size_t descriptor_length,
    const interop_shm_header_snapshot_t *header,
    uint32_t descriptor_index,
    uint64_t expected_window,
    uint64_t expected_generation,
    interop_slot_snapshot_t *snapshot);

/**
 * @brief Compute clamped BGRA frame geometry and page-rounded capacity.
 * @param[in] requested_width Requested nonzero width in physical pixels.
 * @param[in] requested_height Requested nonzero height in physical pixels.
 * @param[out] frame_layout Non-NULL aligned caller-owned output.
 * @return Ok on success, InvalidArgument for null or misaligned output, or
 *         InvalidLayout for zero geometry or an unrepresentable layout.
 * @note Allocation-free and thread-safe for disjoint output storage. Output is
 *       unchanged on failure; caller owns the output value.
 */
interop_layout_status_t interop_layout_frame_layout_for_capture(
    uint32_t requested_width,
    uint32_t requested_height,
    interop_frame_layout_t *frame_layout);

/**
 * @brief Return fixed PCM bounds for one layout-v3 audio ring.
 * @param[in] mapping_length Actual mapping size, expected to be 2 GiB.
 * @param[in] ring_index Ring index in [0, 8).
 * @param[out] bounds Non-NULL aligned caller-owned output.
 * @return Ok on success, InvalidArgument for null or misaligned output, or
 *         InvalidLayout for wrong mapping length or ring index.
 * @note Allocation-free and thread-safe for disjoint output storage. Offsets are
 *       borrowed mapping-relative values; output is unchanged on failure.
 */
interop_layout_status_t interop_layout_audio_ring_bounds(
    uint64_t mapping_length,
    uint32_t ring_index,
    interop_audio_ring_bounds_t *bounds);

#endif
