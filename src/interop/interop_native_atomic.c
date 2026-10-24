/**
 * @file interop_native_atomic.c
 * @brief C11 atomics for frozen layout-v3 descriptor ownership and heartbeats.
 *
 * This module only validates pointer facts and performs native atomic operations.
 * It does not own/map/initialize shared storage or validate non-atomic metadata.
 */
#include "interop_native_atomic.h"

#include <stdbool.h>

typedef struct interop_address_range_t {
    uintptr_t start;
    uintptr_t end;
} interop_address_range_t;

static bool checked_address_range(uintptr_t start, uint64_t length, interop_address_range_t *range) {
    if (start == 0 || length == 0 || length > (uint64_t)UINTPTR_MAX) {
        return false;
    }
    const uintptr_t native_length = (uintptr_t)length;
    if (start > UINTPTR_MAX - native_length) {
        return false;
    }
    range->start = start;
    range->end = start + native_length;
    return true;
}

static bool ranges_overlap(interop_address_range_t left, interop_address_range_t right) {
    return left.start < right.end && right.start < left.end;
}

static bool valid_mapping(uintptr_t mapping_base, uint64_t mapping_length,
                          interop_address_range_t *mapping_range) {
    if (mapping_length != InteropNativeMappingBytes ||
        mapping_base % InteropShmAlignment != 0) {
        return false;
    }
    return checked_address_range(mapping_base, mapping_length, mapping_range);
}

static bool pointer_is_aligned(uintptr_t address, size_t alignment) {
    return address != 0 && address % alignment == 0;
}

static bool header_is_in_mapping(const interop_shm_header_t *header,
                                 uintptr_t mapping_base,
                                 interop_address_range_t mapping_range) {
    const uintptr_t header_address = (uintptr_t)header;
    interop_address_range_t header_range;
    return header_address == mapping_base &&
           pointer_is_aligned(header_address, _Alignof(interop_shm_header_t)) &&
           checked_address_range(header_address, sizeof(*header), &header_range) &&
           header_range.end <= mapping_range.end;
}

static bool descriptor_is_in_table(const interop_slot_descriptor_t *descriptor,
                                    uintptr_t mapping_base,
                                    interop_address_range_t mapping_range) {
    const uintptr_t descriptor_address = (uintptr_t)descriptor;
    interop_address_range_t descriptor_range;
    if (!pointer_is_aligned(descriptor_address, _Alignof(interop_slot_descriptor_t)) ||
        !checked_address_range(descriptor_address, sizeof(*descriptor), &descriptor_range) ||
        descriptor_address < mapping_base + InteropNativeDescriptorOffset ||
        descriptor_range.end > mapping_range.end) {
        return false;
    }

    const uintptr_t relative_offset = descriptor_address - mapping_base - InteropNativeDescriptorOffset;
    const uintptr_t table_bytes = (uintptr_t)InteropNativeDescriptorCount * sizeof(*descriptor);
    return relative_offset < table_bytes && relative_offset % sizeof(*descriptor) == 0;
}

static bool allowed_transition(uint32_t from_state, uint32_t to_state) {
    return (from_state == InteropSlotFree && to_state == InteropSlotWriting) ||
           (from_state == InteropSlotWriting && to_state == InteropSlotReady) ||
           (from_state == InteropSlotReady && to_state == InteropSlotConsuming) ||
           (from_state == InteropSlotConsuming && to_state == InteropSlotFree) ||
           (from_state == InteropSlotWriting && to_state == InteropSlotFree);
}

static _Atomic uint64_t *heartbeat_for_kind(interop_shm_header_t *header,
                                            interop_heartbeat_kind_t kind) {
    switch (kind) {
    case InteropGuestHeartbeat:
        return &header->guest_heartbeat;
    case InteropHostHeartbeat:
        return &header->host_heartbeat;
    default:
        return NULL;
    }
}

interop_atomic_status_t interop_atomic_validate_mapping(
    uintptr_t mapping_base,
    uint64_t mapping_length,
    const interop_shm_header_t *header,
    const interop_slot_descriptor_t *descriptor) {
    interop_address_range_t mapped_range;
    if (!valid_mapping(mapping_base, mapping_length, &mapped_range) ||
        !header_is_in_mapping(header, mapping_base, mapped_range) ||
        !descriptor_is_in_table(descriptor, mapping_base, mapped_range)) {
        return InteropAtomicStatusInvalidArgument;
    }

    if (!atomic_is_lock_free(&header->guest_heartbeat) ||
        !atomic_is_lock_free(&header->host_heartbeat) ||
        !atomic_is_lock_free(&descriptor->state)) {
        return InteropAtomicStatusNotLockFree;
    }
    return InteropAtomicStatusOk;
}

interop_atomic_status_t interop_slot_transition(
    interop_slot_descriptor_t *descriptor,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    uint32_t from_state,
    uint32_t to_state) {
    if (!allowed_transition(from_state, to_state)) {
        return InteropAtomicStatusInvalidArgument;
    }

    interop_address_range_t mapped_range;
    if (!valid_mapping(mapping_base, mapping_length, &mapped_range) ||
        !descriptor_is_in_table(descriptor, mapping_base, mapped_range)) {
        return InteropAtomicStatusInvalidArgument;
    }
    if (!atomic_is_lock_free(&descriptor->state)) {
        return InteropAtomicStatusNotLockFree;
    }

    uint32_t observed_state = from_state;
    if (!atomic_compare_exchange_strong_explicit(
            &descriptor->state,
            &observed_state,
            to_state,
            memory_order_acq_rel,
            memory_order_relaxed)) {
        return InteropAtomicStatusWrongState;
    }
    return InteropAtomicStatusOk;
}

interop_atomic_status_t interop_heartbeat_load(
    const interop_shm_header_t *header,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    interop_heartbeat_kind_t kind,
    uint64_t *value) {
    interop_address_range_t mapped_range;
    interop_address_range_t value_range;
    if (kind != InteropGuestHeartbeat && kind != InteropHostHeartbeat) {
        return InteropAtomicStatusInvalidArgument;
    }
    if (!valid_mapping(mapping_base, mapping_length, &mapped_range) ||
        !header_is_in_mapping(header, mapping_base, mapped_range) ||
        !pointer_is_aligned((uintptr_t)value, _Alignof(uint64_t)) ||
        !checked_address_range((uintptr_t)value, sizeof(*value), &value_range) ||
        ranges_overlap(mapped_range, value_range)) {
        return InteropAtomicStatusInvalidArgument;
    }

    const _Atomic uint64_t *counter = kind == InteropGuestHeartbeat
        ? &header->guest_heartbeat
        : &header->host_heartbeat;
    if (!atomic_is_lock_free(counter)) {
        return InteropAtomicStatusNotLockFree;
    }

    const uint64_t result = atomic_load_explicit(counter, memory_order_relaxed);
    *value = result;
    return InteropAtomicStatusOk;
}

interop_atomic_status_t interop_heartbeat_store_next(
    interop_shm_header_t *header,
    uintptr_t mapping_base,
    uint64_t mapping_length,
    interop_heartbeat_kind_t kind,
    uint64_t next_value) {
    if ((kind != InteropGuestHeartbeat && kind != InteropHostHeartbeat) || next_value == 0) {
        return InteropAtomicStatusInvalidArgument;
    }

    interop_address_range_t mapped_range;
    if (!valid_mapping(mapping_base, mapping_length, &mapped_range) ||
        !header_is_in_mapping(header, mapping_base, mapped_range)) {
        return InteropAtomicStatusInvalidArgument;
    }

    _Atomic uint64_t *counter = heartbeat_for_kind(header, kind);
    if (counter == NULL) {
        return InteropAtomicStatusInvalidArgument;
    }
    if (!atomic_is_lock_free(counter)) {
        return InteropAtomicStatusNotLockFree;
    }

    const uint64_t current_value = atomic_load_explicit(counter, memory_order_relaxed);
    if (current_value == UINT64_MAX) {
        return InteropAtomicStatusCounterExhausted;
    }
    if (next_value <= current_value) {
        return InteropAtomicStatusWrongState;
    }
    atomic_store_explicit(counter, next_value, memory_order_relaxed);
    return InteropAtomicStatusOk;
}
