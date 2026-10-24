/**
 * @file layout_abi.c
 * @brief C consumer checks for the frozen layout-v3 snapshot ABI.
 *
 * All supplied bytes are private fixtures. Fake pointer values occur only on
 * rejection paths where the ABI must reject the address range before access.
 */
#include "interop_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "layout_abi.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void write_u32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = (uint8_t)value;
    bytes[offset + 1] = (uint8_t)(value >> 8);
    bytes[offset + 2] = (uint8_t)(value >> 16);
    bytes[offset + 3] = (uint8_t)(value >> 24);
}

static void write_u64(uint8_t *bytes, size_t offset, uint64_t value) {
    write_u32(bytes, offset, (uint32_t)value);
    write_u32(bytes, offset + 4, (uint32_t)(value >> 32));
}

static void make_header(uint8_t bytes[4096], uint64_t epoch) {
    memset(bytes, 0, 4096);
    write_u32(bytes, 0, UINT32_C(0x33574d53));
    write_u32(bytes, 4, 3);
    write_u64(bytes, 8, epoch);
    write_u64(bytes, 16, UINT64_C(0x80000000));
    write_u32(bytes, 24, 192);
    write_u32(bytes, 28, 8);
    write_u64(bytes, 32, UINT64_C(0x200000));
    write_u64(bytes, 40, UINT64_C(0x7fc00000));
    write_u32(bytes, 48, UINT32_C(0x200000));
}

static void make_descriptor(uint8_t bytes[64]) {
    memset(bytes, 0, 64);
    write_u32(bytes, 0, 2);
    write_u32(bytes, 4, UINT32_C(0x34325241));
    write_u64(bytes, 8, UINT64_C(0x1234));
    write_u64(bytes, 16, 9);
    write_u64(bytes, 24, 71);
    write_u64(bytes, 32, UINT64_C(0x200000));
    write_u32(bytes, 40, UINT32_C(0x400000));
    write_u32(bytes, 44, 640);
    write_u32(bytes, 48, 480);
    write_u32(bytes, 52, 2560);
}

int main(void) {
    _Alignas(64) uint8_t header_bytes[4096];
    _Alignas(8) uint8_t descriptor_bytes[64];
    _Alignas(64) interop_shm_header_snapshot_t header_snapshot;
    interop_shm_header_snapshot_t header_before;
    interop_slot_snapshot_t slot_snapshot;
    interop_slot_snapshot_t slot_before;
    interop_frame_layout_t frame_layout;
    interop_frame_layout_t frame_before;
    interop_audio_ring_bounds_t ring_bounds;
    interop_audio_ring_bounds_t ring_before;

    memset(&header_snapshot, 0xa5, sizeof(header_snapshot));
    header_before = header_snapshot;
    make_header(header_bytes, UINT64_C(0x1122334455667788));
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), UINT64_C(0x1122334455667788),
        &header_snapshot) == InteropLayoutStatusOk);
    Check(header_snapshot.session_epoch == UINT64_C(0x1122334455667788));
    Check(header_snapshot.heap_bytes == UINT64_C(0x7fc00000));
    header_snapshot = header_before;
    Check(interop_layout_validate_shm_header(
        NULL, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), 1, &header_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), 1, NULL) == InteropLayoutStatusInvalidArgument);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes) - 1, (uintptr_t)0x4000,
        UINT64_C(0x80000000), 1, &header_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), UINTPTR_MAX - (uintptr_t)63,
        UINT64_C(0x80000000), 1, &header_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), 1,
        (interop_shm_header_snapshot_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)63)) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)header_bytes,
        UINT64_C(0x80000000), UINT64_C(0x1122334455667788),
        &header_snapshot) == InteropLayoutStatusInvalidArgument);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)&header_snapshot,
        UINT64_C(0x80000000), UINT64_C(0x1122334455667788),
        &header_snapshot) == InteropLayoutStatusInvalidArgument);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), 1,
        (interop_shm_header_snapshot_t *)((uint8_t *)&header_snapshot + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    header_bytes[0] ^= 1;
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), UINT64_C(0x1122334455667788),
        &header_snapshot) == InteropLayoutStatusInvalidLayout);
    Check(memcmp(&header_snapshot, &header_before, sizeof(header_snapshot)) == 0);
    make_header(header_bytes, UINT64_C(0x1122334455667788));

    make_descriptor(descriptor_bytes);
    memset(&slot_snapshot, 0x5a, sizeof(slot_snapshot));
    slot_before = slot_snapshot;
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), &header_before, 3,
        UINT64_C(0x1234), 9, &slot_snapshot) == InteropLayoutStatusInvalidLayout);
    Check(memcmp(&slot_snapshot, &slot_before, sizeof(slot_snapshot)) == 0);
    Check(interop_layout_validate_shm_header(
        header_bytes, sizeof(header_bytes), (uintptr_t)0x4000,
        UINT64_C(0x80000000), UINT64_C(0x1122334455667788),
        &header_snapshot) == InteropLayoutStatusOk);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), &header_snapshot, 3,
        UINT64_C(0x1234), 9, &slot_snapshot) == InteropLayoutStatusOk);
    Check(slot_snapshot.descriptor_index == 3 && slot_snapshot.width == 640);
    slot_before = slot_snapshot;
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), &header_snapshot, 3,
        UINT64_C(0x1234), 9, NULL) == InteropLayoutStatusInvalidArgument);
    Check(memcmp(&slot_snapshot, &slot_before, sizeof(slot_snapshot)) == 0);
    Check(interop_layout_validate_slot_descriptor(
        NULL, sizeof(descriptor_bytes), &header_snapshot, 3,
        UINT64_C(0x1234), 9, &slot_snapshot) == InteropLayoutStatusInvalidArgument);
    Check(memcmp(&slot_snapshot, &slot_before, sizeof(slot_snapshot)) == 0);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes) - 1, &header_snapshot, 3,
        UINT64_C(0x1234), 9, &slot_snapshot) == InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), NULL, 3,
        UINT64_C(0x1234), 9, &slot_snapshot) == InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), &header_snapshot, 3,
        UINT64_C(0x1234), 9,
        (interop_slot_snapshot_t *)((uint8_t *)&slot_snapshot + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes),
        (const interop_shm_header_snapshot_t *)((const uint8_t *)&header_snapshot + 1),
        3, UINT64_C(0x1234), 9, &slot_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_slot_descriptor(
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)63), 64,
        &header_snapshot, 3, UINT64_C(0x1234), 9, &slot_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_validate_slot_descriptor(
        descriptor_bytes, sizeof(descriptor_bytes), &header_snapshot, 3,
        UINT64_C(0x1234), 9, (interop_slot_snapshot_t *)&header_snapshot) ==
        InteropLayoutStatusInvalidArgument);
    Check(memcmp(&slot_snapshot, &slot_before, sizeof(slot_snapshot)) == 0);

    memset(&frame_layout, 0x3c, sizeof(frame_layout));
    Check(interop_layout_frame_layout_for_capture(8000, 5000, &frame_layout) ==
        InteropLayoutStatusOk);
    Check(frame_layout.width == 7680 && frame_layout.height == 4320);
    Check(frame_layout.stride == 30720 && frame_layout.data_bytes == UINT32_C(0x8000000));
    frame_before = frame_layout;
    Check(interop_layout_frame_layout_for_capture(0, 1, &frame_layout) ==
        InteropLayoutStatusInvalidLayout);
    Check(interop_layout_frame_layout_for_capture(1, 1, NULL) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_frame_layout_for_capture(1, 1,
        (interop_frame_layout_t *)((uint8_t *)&frame_layout + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_frame_layout_for_capture(1, 1,
        (interop_frame_layout_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)sizeof(frame_layout) + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(memcmp(&frame_layout, &frame_before, sizeof(frame_layout)) == 0);

    memset(&ring_bounds, 0x69, sizeof(ring_bounds));
    Check(interop_layout_audio_ring_bounds(UINT64_C(0x80000000), 7, &ring_bounds) ==
        InteropLayoutStatusOk);
    Check(ring_bounds.ring_index == 7 && ring_bounds.header_offset == UINT64_C(0xf0000));
    Check(ring_bounds.pcm_bytes == 65536 && ring_bounds.capacity_frames == 16384);
    ring_before = ring_bounds;
    Check(interop_layout_audio_ring_bounds(UINT64_C(0x80000000), 8, &ring_bounds) ==
        InteropLayoutStatusInvalidLayout);
    Check(interop_layout_audio_ring_bounds(0, 0, &ring_bounds) ==
        InteropLayoutStatusInvalidLayout);
    Check(interop_layout_audio_ring_bounds(UINT64_C(0x80000000), 0, NULL) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_audio_ring_bounds(UINT64_C(0x80000000), 0,
        (interop_audio_ring_bounds_t *)((uint8_t *)&ring_bounds + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(interop_layout_audio_ring_bounds(UINT64_C(0x80000000), 0,
        (interop_audio_ring_bounds_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)sizeof(ring_bounds) + 1)) ==
        InteropLayoutStatusInvalidArgument);
    Check(memcmp(&ring_bounds, &ring_before, sizeof(ring_bounds)) == 0);
    return 0;
}
