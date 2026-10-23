#ifndef WaddleInteropMemoryH
#define WaddleInteropMemoryH
/**
 * @file interop_memory.h
 * @brief Layout v3 ABI declarations; allocator and mapping validation are TODO #5.
 *
 * All storage is borrowed from the device's 2 GiB little-endian mapping, aligned
 * to 64 bytes. No declarations allocate or free memory. The guest initializes
 * the mapping before Hello and owns descriptor metadata while state is Writing.
 * Release publication of Ready and acquire consumption protect those fields;
 * host release of Free permits reuse. Heartbeats use atomic loads/stores.
 * The session owns the mapping until both peers and compositor buffers detach.
 */
#include "interop_protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/** @brief Layout v3 magic, little-endian bytes SMW3. */
#define InteropShmMagic 0x33574D53u
/** @brief Shared header bytes, including reserved storage. */
#define InteropShmHeaderBytes 4096u
/** @brief Mapping and descriptor alignment in bytes. */
#define InteropShmAlignment 64u

/** @brief Guest-initialized header; non-NULL borrowed mapping storage.
 * @note The guest owns all fields except the host-written host_heartbeat.
 * Non-atomic fields are immutable during a connected session. Thread-safe
 * heartbeat access requires C atomic operations; reserved bytes stay zero.
 */
typedef struct interop_shm_header_t {
    _Alignas(64) uint32_t magic; /**< InteropShmMagic; offset 0. */
    uint32_t layout_version; /**< InteropLayoutVersion; offset 4. */
    uint64_t session_epoch; /**< Nonzero Hello epoch; offset 8. */
    uint64_t total_bytes; /**< Mapping length, 0x80000000; offset 16. */
    uint32_t slot_count; /**< 192 descriptors; offset 24. */
    uint32_t audio_ring_count; /**< Eight audio rings; offset 28. */
    uint64_t heap_offset; /**< Heap begins at 0x00200000; offset 32. */
    uint64_t heap_bytes; /**< 0x7FC00000 bytes; offset 40. */
    uint32_t page_bytes; /**< 0x00200000-byte allocation unit; offset 48. */
    uint32_t reserved0; /**< Zero; offset 52. */
    _Atomic uint64_t guest_heartbeat; /**< Guest increments every 500 ms; offset 56. */
    _Atomic uint64_t host_heartbeat; /**< Host increments every 500 ms; offset 64. */
    uint8_t reserved[4096 - 72]; /**< Zero through byte 4095; offset 72. */
} interop_shm_header_t;

/** @brief One guest-owned frame descriptor, non-NULL borrowed mapping storage.
 * @note State transfers access between capture producer and host consumer:
 * Free=0, Writing=1, Ready=2, Consuming=3. All metadata is immutable after
 * release-publication of Ready until host release of Free. The guest may free
 * pixel pages only after every consuming compositor buffer releases ownership.
 */
typedef struct interop_slot_descriptor_t {
    _Alignas(64) _Atomic uint32_t state; /**< Ownership state; offset 0. */
    uint32_t format; /**< Premultiplied BGRA, AvPixelFormat; offset 4. */
    uint64_t window; /**< HWND, zero when unassigned; offset 8. */
    uint64_t generation; /**< Window incarnation; offset 16. */
    uint64_t frame_sequence; /**< Nonzero frame identifier; offset 24. */
    uint64_t data_offset; /**< Mapping-relative page-aligned pixel offset; offset 32. */
    uint32_t data_bytes; /**< Capacity, a page multiple; offset 40. */
    uint32_t width; /**< Physical pixel width; offset 44. */
    uint32_t height; /**< Physical pixel height; offset 48. */
    uint32_t stride; /**< Row bytes, multiple of 64 and at least width*4; offset 52. */
    uint64_t reserved; /**< Zero; offset 56. */
} interop_slot_descriptor_t;

_Static_assert(sizeof(interop_shm_header_t) == InteropShmHeaderBytes, "interop header size");
_Static_assert(_Alignof(interop_shm_header_t) == InteropShmAlignment, "interop header alignment");
_Static_assert(sizeof(interop_slot_descriptor_t) == 64, "interop slot size");
_Static_assert(_Alignof(interop_slot_descriptor_t) == InteropShmAlignment, "interop slot alignment");
_Static_assert(offsetof(interop_shm_header_t, magic) == 0, "header magic offset");
_Static_assert(offsetof(interop_shm_header_t, layout_version) == 4, "header version offset");
_Static_assert(offsetof(interop_shm_header_t, session_epoch) == 8, "header epoch offset");
_Static_assert(offsetof(interop_shm_header_t, total_bytes) == 16, "header length offset");
_Static_assert(offsetof(interop_shm_header_t, slot_count) == 24, "header slots offset");
_Static_assert(offsetof(interop_shm_header_t, audio_ring_count) == 28, "header rings offset");
_Static_assert(offsetof(interop_shm_header_t, heap_offset) == 32, "header heap offset");
_Static_assert(offsetof(interop_shm_header_t, heap_bytes) == 40, "header heap length offset");
_Static_assert(offsetof(interop_shm_header_t, page_bytes) == 48, "header page size offset");
_Static_assert(offsetof(interop_shm_header_t, reserved0) == 52, "header reserved word offset");
_Static_assert(offsetof(interop_shm_header_t, guest_heartbeat) == 56, "guest heartbeat offset");
_Static_assert(offsetof(interop_shm_header_t, host_heartbeat) == 64, "host heartbeat offset");
_Static_assert(offsetof(interop_shm_header_t, reserved) == 72, "header reserved bytes offset");
_Static_assert(offsetof(interop_slot_descriptor_t, state) == 0, "slot state offset");
_Static_assert(offsetof(interop_slot_descriptor_t, format) == 4, "slot format offset");
_Static_assert(offsetof(interop_slot_descriptor_t, window) == 8, "slot window offset");
_Static_assert(offsetof(interop_slot_descriptor_t, generation) == 16, "slot generation offset");
_Static_assert(offsetof(interop_slot_descriptor_t, frame_sequence) == 24, "slot sequence offset");
_Static_assert(offsetof(interop_slot_descriptor_t, data_offset) == 32, "slot data offset");
_Static_assert(offsetof(interop_slot_descriptor_t, data_bytes) == 40, "slot capacity offset");
_Static_assert(offsetof(interop_slot_descriptor_t, width) == 44, "slot width offset");
_Static_assert(offsetof(interop_slot_descriptor_t, height) == 48, "slot height offset");
_Static_assert(offsetof(interop_slot_descriptor_t, stride) == 52, "slot stride offset");
_Static_assert(offsetof(interop_slot_descriptor_t, reserved) == 56, "slot reserved offset");
#endif
