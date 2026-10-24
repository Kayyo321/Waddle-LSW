/**
 * @file native_atomic.c
 * @brief Linux pthread and Windows CreateThread tests for layout-v3 atomics.
 *
 * The fixture reserves the frozen 2 GiB virtual address range, then commits or
 * faults in only the header/descriptor pages it touches. No pixel heap is
 * initialized or made resident. It validates state transitions and ownership;
 * it does not claim mapping initialization or compositor-buffer lifetime.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "interop_native_atomic.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE native_thread_t;
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
typedef pthread_t native_thread_t;
#endif

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native_atomic.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define CheckMain(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native_atomic.c:%d: %s\n", __LINE__, #condition); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

#define StressIterations 100000u
#define SpinLimit 10000000u
#define CancelMarker UINT64_C(0xcace11ed12345678)
#define GenerationBase UINT64_C(0x100000000)
#define FormatBgra UINT32_C(0x34325241)
#define PixelOffset UINT64_C(0x00200000)
#define PixelBytes UINT32_C(0x00200000)
#define FrameWidth 640u
#define FrameHeight 480u
#define FrameStride 2560u

typedef struct stress_context_t {
    interop_slot_descriptor_t *descriptor;
    uintptr_t mapping_base;
    _Atomic uint32_t failed;
    _Atomic uint32_t cancel_requested;
    _Atomic uint32_t cancel_acknowledged;
    _Atomic uint32_t consumed_frames;
    uint64_t published_checksum;
} stress_context_t;

static void *reserve_mapping(void) {
#ifdef _WIN32
    void *mapping = VirtualAlloc(NULL, (SIZE_T)InteropNativeMappingBytes,
                                 MEM_RESERVE, PAGE_NOACCESS);
    if (mapping == NULL) {
        return NULL;
    }
    if (VirtualAlloc(mapping, 0x4000, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        VirtualFree(mapping, 0, MEM_RELEASE);
        return NULL;
    }
    memset(mapping, 0, 0x4000);
    return mapping;
#else
    void *mapping = mmap(NULL, (size_t)InteropNativeMappingBytes,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                         -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    memset(mapping, 0, 0x4000);
    return mapping;
#endif
}

static void release_mapping(void *mapping) {
    if (mapping == NULL) {
        return;
    }
#ifdef _WIN32
    (void)VirtualFree(mapping, 0, MEM_RELEASE);
#else
    (void)munmap(mapping, (size_t)InteropNativeMappingBytes);
#endif
}

static uint64_t descriptor_checksum(const interop_slot_descriptor_t *descriptor) {
    uint64_t checksum = descriptor->window;
    checksum ^= descriptor->generation * UINT64_C(0x9e3779b185ebca87);
    checksum ^= descriptor->frame_sequence * UINT64_C(0xc2b2ae3d27d4eb4f);
    checksum ^= descriptor->data_offset;
    checksum ^= (uint64_t)descriptor->format << 33;
    checksum ^= (uint64_t)descriptor->data_bytes << 17;
    checksum ^= (uint64_t)descriptor->width << 49;
    checksum ^= (uint64_t)descriptor->height << 9;
    checksum ^= (uint64_t)descriptor->stride << 1;
    checksum ^= descriptor->reserved;
    return checksum;
}

static void mark_failed(stress_context_t *context) {
    atomic_store_explicit(&context->failed, 1, memory_order_release);
}

static bool claim_transition(stress_context_t *context,
                             uint32_t from_state,
                             uint32_t to_state) {
    for (uint32_t spin = 0; spin < SpinLimit; ++spin) {
        if (atomic_load_explicit(&context->failed, memory_order_acquire) != 0) {
            return false;
        }
        const interop_atomic_status_t status = interop_slot_transition(
            context->descriptor,
            context->mapping_base,
            InteropNativeMappingBytes,
            from_state,
            to_state);
        if (status == InteropAtomicStatusOk) {
            return true;
        }
        if (status != InteropAtomicStatusWrongState) {
            mark_failed(context);
            return false;
        }
    }
    mark_failed(context);
    return false;
}

static void *producer_main(void *argument) {
    stress_context_t *context = argument;
    interop_slot_descriptor_t *descriptor = context->descriptor;
    const uintptr_t mapping_base = context->mapping_base;

    if (!claim_transition(context, InteropSlotFree, InteropSlotWriting)) {
        return NULL;
    }
    descriptor->format = FormatBgra;
    descriptor->window = CancelMarker;
    descriptor->generation = CancelMarker ^ 1;
    descriptor->frame_sequence = 0;
    descriptor->data_offset = PixelOffset;
    descriptor->data_bytes = PixelBytes;
    descriptor->width = FrameWidth;
    descriptor->height = FrameHeight;
    descriptor->stride = FrameStride;
    descriptor->reserved = 0;
    context->published_checksum = descriptor_checksum(descriptor);
    if (interop_slot_transition(descriptor, mapping_base, InteropNativeMappingBytes,
                               InteropSlotWriting, InteropSlotFree) != InteropAtomicStatusOk) {
        mark_failed(context);
        return NULL;
    }
    atomic_store_explicit(&context->cancel_requested, 1, memory_order_release);

    bool cancel_acknowledged = false;
    for (uint32_t spin = 0; spin < SpinLimit; ++spin) {
        if (atomic_load_explicit(&context->failed, memory_order_acquire) != 0) {
            return NULL;
        }
        if (atomic_load_explicit(&context->cancel_acknowledged, memory_order_acquire) != 0) {
            cancel_acknowledged = true;
            break;
        }
    }
    if (!cancel_acknowledged) {
        mark_failed(context);
        return NULL;
    }

    for (uint32_t frame_index = 0; frame_index < StressIterations; ++frame_index) {
        if (!claim_transition(context, InteropSlotFree, InteropSlotWriting)) {
            return NULL;
        }
        descriptor->format = FormatBgra;
        descriptor->window = (uint64_t)frame_index + 1;
        descriptor->generation = GenerationBase + frame_index;
        descriptor->frame_sequence = (uint64_t)frame_index + 1;
        descriptor->data_offset = PixelOffset;
        descriptor->data_bytes = PixelBytes;
        descriptor->width = FrameWidth;
        descriptor->height = FrameHeight;
        descriptor->stride = FrameStride;
        descriptor->reserved = 0;
        context->published_checksum = descriptor_checksum(descriptor);
        if (interop_slot_transition(descriptor, mapping_base, InteropNativeMappingBytes,
                                    InteropSlotWriting, InteropSlotReady) != InteropAtomicStatusOk) {
            mark_failed(context);
            return NULL;
        }
        /* No descriptor metadata is read or written until a later Free->Writing claim. */
    }
    return NULL;
}

static void *consumer_main(void *argument) {
    stress_context_t *context = argument;
    interop_slot_descriptor_t *descriptor = context->descriptor;
    const uintptr_t mapping_base = context->mapping_base;

    bool cancel_request_seen = false;
    for (uint32_t spin = 0; spin < SpinLimit; ++spin) {
        if (atomic_load_explicit(&context->failed, memory_order_acquire) != 0) {
            return NULL;
        }
        if (atomic_load_explicit(&context->cancel_requested, memory_order_acquire) != 0) {
            cancel_request_seen = true;
            break;
        }
    }
    if (!cancel_request_seen || !claim_transition(context, InteropSlotFree, InteropSlotWriting)) {
        mark_failed(context);
        return NULL;
    }

    const bool cancellation_visible = descriptor->window == CancelMarker &&
        descriptor->generation == (CancelMarker ^ 1) && descriptor->frame_sequence == 0 &&
        context->published_checksum == descriptor_checksum(descriptor);
    const interop_atomic_status_t cancel_release = interop_slot_transition(
        descriptor, mapping_base, InteropNativeMappingBytes,
        InteropSlotWriting, InteropSlotFree);
    if (!cancellation_visible || cancel_release != InteropAtomicStatusOk) {
        mark_failed(context);
        return NULL;
    }
    atomic_store_explicit(&context->cancel_acknowledged, 1, memory_order_release);

    uint32_t completed_frames = 0;
    while (completed_frames < StressIterations &&
           atomic_load_explicit(&context->failed, memory_order_acquire) == 0) {
        if (!claim_transition(context, InteropSlotReady, InteropSlotConsuming)) {
            return NULL;
        }

        const uint64_t sequence = descriptor->frame_sequence;
        const uint64_t frame_index = sequence == 0 ? UINT64_MAX : sequence - 1;
        const uint64_t observed_checksum = descriptor_checksum(descriptor);
        const bool metadata_valid = sequence == (uint64_t)completed_frames + 1 && frame_index < StressIterations &&
            descriptor->format == FormatBgra && descriptor->window == frame_index + 1 &&
            descriptor->generation == GenerationBase + frame_index &&
            descriptor->data_offset == PixelOffset && descriptor->data_bytes == PixelBytes &&
            descriptor->width == FrameWidth && descriptor->height == FrameHeight &&
            descriptor->stride == FrameStride && descriptor->reserved == 0 &&
            context->published_checksum == observed_checksum;

        const interop_atomic_status_t release_status = interop_slot_transition(
            descriptor, mapping_base, InteropNativeMappingBytes,
            InteropSlotConsuming, InteropSlotFree);
        if (release_status != InteropAtomicStatusOk) {
            mark_failed(context);
            return NULL;
        }
        /* Descriptor metadata is no longer accessed after the release to Free. */
        if (!metadata_valid) {
            mark_failed(context);
            return NULL;
        }
        ++completed_frames;
        atomic_store_explicit(&context->consumed_frames, completed_frames, memory_order_release);
    }
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI producer_thread_entry(LPVOID argument) {
    (void)producer_main(argument);
    return 0;
}

static DWORD WINAPI consumer_thread_entry(LPVOID argument) {
    (void)consumer_main(argument);
    return 0;
}

static bool start_threads(stress_context_t *context, native_thread_t *consumer,
                          native_thread_t *producer) {
    *consumer = CreateThread(NULL, 0, consumer_thread_entry, context, 0, NULL);
    if (*consumer == NULL) {
        return false;
    }
    *producer = CreateThread(NULL, 0, producer_thread_entry, context, 0, NULL);
    if (*producer == NULL) {
        mark_failed(context);
        WaitForSingleObject(*consumer, INFINITE);
        CloseHandle(*consumer);
        *consumer = NULL;
        return false;
    }
    return true;
}

static void join_threads(native_thread_t consumer, native_thread_t producer) {
    if (consumer != NULL) {
        WaitForSingleObject(consumer, INFINITE);
        CloseHandle(consumer);
    }
    if (producer != NULL) {
        WaitForSingleObject(producer, INFINITE);
        CloseHandle(producer);
    }
}
#else
static bool start_threads(stress_context_t *context, native_thread_t *consumer,
                          native_thread_t *producer) {
    if (pthread_create(consumer, NULL, consumer_main, context) != 0) {
        return false;
    }
    if (pthread_create(producer, NULL, producer_main, context) != 0) {
        mark_failed(context);
        pthread_join(*consumer, NULL);
        return false;
    }
    return true;
}

static void join_threads(native_thread_t consumer, native_thread_t producer) {
    (void)pthread_join(consumer, NULL);
    (void)pthread_join(producer, NULL);
}
#endif

static interop_shm_header_t *initialize_mapping(void *mapping) {
    interop_shm_header_t *header = mapping;
    atomic_init(&header->guest_heartbeat, 0);
    atomic_init(&header->host_heartbeat, 0);
    return header;
}

static int test_invalid_arguments(void *mapping,
                                  interop_shm_header_t *header,
                                  interop_slot_descriptor_t *descriptor) {
    const uintptr_t mapping_base = (uintptr_t)mapping;
    uint64_t heartbeat_value = UINT64_C(0xaabbccdd);
    const uint64_t heartbeat_before = heartbeat_value;

    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        NULL, descriptor) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes - 1,
        header, descriptor) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(0, InteropNativeMappingBytes,
        (const interop_shm_header_t *)(uintptr_t)1,
        (const interop_slot_descriptor_t *)(uintptr_t)2) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base + 1, InteropNativeMappingBytes,
        header, descriptor) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        (const interop_shm_header_t *)(mapping_base + InteropShmAlignment), descriptor) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(UINTPTR_MAX - (uintptr_t)63,
        InteropNativeMappingBytes, (const interop_shm_header_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)63),
        (const interop_slot_descriptor_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)63)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeDescriptorOffset +
            InteropNativeDescriptorCount * sizeof(*descriptor))) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeDescriptorOffset + 1)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)mapping_base) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeDescriptorOffset +
            InteropNativeDescriptorCount * sizeof(*descriptor))) == InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeMappingBytes - 32)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeMappingBytes)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)63)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_atomic_validate_mapping(mapping_base, InteropNativeMappingBytes,
        header, (const interop_slot_descriptor_t *)(mapping_base + InteropNativeDescriptorOffset +
            191 * sizeof(*descriptor))) == InteropAtomicStatusOk);

    Check(interop_slot_transition((interop_slot_descriptor_t *)(uintptr_t)1,
        mapping_base, InteropNativeMappingBytes, InteropSlotFree, InteropSlotReady) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition(NULL, mapping_base, InteropNativeMappingBytes,
        InteropSlotFree, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition(descriptor, mapping_base, InteropNativeMappingBytes,
        UINT32_MAX, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition(descriptor, mapping_base, InteropNativeMappingBytes,
        InteropSlotWriting, InteropSlotReady) == InteropAtomicStatusWrongState);
    Check(interop_slot_transition((interop_slot_descriptor_t *)(mapping_base +
        InteropNativeDescriptorOffset + InteropNativeDescriptorCount * sizeof(*descriptor)),
        mapping_base, InteropNativeMappingBytes, InteropSlotFree, InteropSlotWriting) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition((interop_slot_descriptor_t *)(mapping_base +
        InteropNativeDescriptorOffset + 1), mapping_base, InteropNativeMappingBytes,
        InteropSlotFree, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition((interop_slot_descriptor_t *)(uintptr_t)(UINTPTR_MAX -
        (uintptr_t)63), mapping_base, InteropNativeMappingBytes,
        InteropSlotFree, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition(descriptor, mapping_base, InteropNativeMappingBytes - 1,
        InteropSlotFree, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);
    Check(interop_slot_transition(descriptor, 0, InteropNativeMappingBytes,
        InteropSlotFree, InteropSlotWriting) == InteropAtomicStatusInvalidArgument);

    Check(interop_heartbeat_load(NULL, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, &heartbeat_value) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, NULL) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        (interop_heartbeat_kind_t)9, &heartbeat_value) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, (uint64_t *)((uint8_t *)&heartbeat_value + 1)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, (uint64_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)7)) ==
        InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, (uint64_t *)&header->session_epoch) == InteropAtomicStatusInvalidArgument);
    uint64_t *heap_value = malloc(sizeof(*heap_value));
    Check(heap_value != NULL);
    *heap_value = heartbeat_before;
    const interop_atomic_status_t heap_load_status = interop_heartbeat_load(
        header, mapping_base, InteropNativeMappingBytes, InteropGuestHeartbeat, heap_value);
    const uint64_t heap_loaded_value = *heap_value;
    free(heap_value);
    Check(heap_load_status == InteropAtomicStatusOk);
    Check(heap_loaded_value == atomic_load_explicit(&header->guest_heartbeat, memory_order_relaxed));
    Check(heartbeat_value == heartbeat_before);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes - 1,
        InteropGuestHeartbeat, &heartbeat_value) == InteropAtomicStatusInvalidArgument);
    Check(heartbeat_value == heartbeat_before);

    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        (interop_heartbeat_kind_t)9, 1) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, 0) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes - 1,
        InteropGuestHeartbeat, 1) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_store_next(NULL, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, 1) == InteropAtomicStatusInvalidArgument);
    Check(interop_heartbeat_load((const interop_shm_header_t *)(mapping_base + InteropShmAlignment),
        mapping_base, InteropNativeMappingBytes, InteropGuestHeartbeat, &heartbeat_value) ==
        InteropAtomicStatusInvalidArgument);
    return 0;
}

static int test_transition_matrix(interop_slot_descriptor_t *descriptor, uintptr_t mapping_base) {
    static const uint32_t states[] = {
        InteropSlotFree,
        InteropSlotWriting,
        InteropSlotReady,
        InteropSlotConsuming,
        UINT32_C(4)
    };
    for (size_t from_index = 0; from_index < sizeof(states) / sizeof(states[0]); ++from_index) {
        for (size_t to_index = 0; to_index < sizeof(states) / sizeof(states[0]); ++to_index) {
            const uint32_t from_state = states[from_index];
            const uint32_t to_state = states[to_index];
            const bool legal =
                (from_state == InteropSlotFree && to_state == InteropSlotWriting) ||
                (from_state == InteropSlotWriting && to_state == InteropSlotReady) ||
                (from_state == InteropSlotReady && to_state == InteropSlotConsuming) ||
                (from_state == InteropSlotConsuming && to_state == InteropSlotFree) ||
                (from_state == InteropSlotWriting && to_state == InteropSlotFree);
            atomic_store_explicit(&descriptor->state, from_state, memory_order_relaxed);
            const interop_atomic_status_t status = interop_slot_transition(
                descriptor, mapping_base, InteropNativeMappingBytes, from_state, to_state);
            Check(status == (legal ? InteropAtomicStatusOk : InteropAtomicStatusInvalidArgument));
            const uint32_t expected_state = legal ? to_state : from_state;
            Check(atomic_load_explicit(&descriptor->state, memory_order_relaxed) == expected_state);
        }
    }
    atomic_store_explicit(&descriptor->state, InteropSlotFree, memory_order_relaxed);
    return 0;
}

static int test_heartbeats(interop_shm_header_t *header, uintptr_t mapping_base) {
    uint64_t value = 123;
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, &value) == InteropAtomicStatusOk);
    Check(value == 0);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, 1) == InteropAtomicStatusOk);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, &value) == InteropAtomicStatusOk && value == 1);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, 1) == InteropAtomicStatusWrongState);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        InteropHostHeartbeat, 9) == InteropAtomicStatusOk);
    Check(interop_heartbeat_load(header, mapping_base, InteropNativeMappingBytes,
        InteropHostHeartbeat, &value) == InteropAtomicStatusOk && value == 9);
    atomic_store_explicit(&header->guest_heartbeat, UINT64_MAX, memory_order_relaxed);
    Check(interop_heartbeat_store_next(header, mapping_base, InteropNativeMappingBytes,
        InteropGuestHeartbeat, UINT64_MAX) == InteropAtomicStatusCounterExhausted);
    Check(atomic_load_explicit(&header->guest_heartbeat, memory_order_relaxed) == UINT64_MAX);
    return 0;
}

static int test_stress(void *mapping, interop_slot_descriptor_t *descriptor) {
    stress_context_t context;
    memset(&context, 0, sizeof(context));
    context.descriptor = descriptor;
    context.mapping_base = (uintptr_t)mapping;
    atomic_init(&context.failed, 0);
    atomic_init(&context.cancel_requested, 0);
    atomic_init(&context.cancel_acknowledged, 0);
    atomic_init(&context.consumed_frames, 0);

    native_thread_t consumer;
    native_thread_t producer;
    if (!start_threads(&context, &consumer, &producer)) {
        fprintf(stderr, "native_atomic.c: thread creation failed\n");
        return 1;
    }
    join_threads(consumer, producer);

    Check(atomic_load_explicit(&context.failed, memory_order_acquire) == 0);
    Check(atomic_load_explicit(&context.cancel_acknowledged, memory_order_acquire) == 1);
    Check(atomic_load_explicit(&context.consumed_frames, memory_order_acquire) == StressIterations);
    Check(atomic_load_explicit(&descriptor->state, memory_order_relaxed) == InteropSlotFree);
    Check(interop_atomic_validate_mapping((uintptr_t)mapping, InteropNativeMappingBytes,
        mapping, descriptor) == InteropAtomicStatusOk);
    return 0;
}

/**
 * @brief Run pointer, transition, heartbeat, and threaded visibility checks.
 * @return Zero when all checks pass; one on the first failed assertion.
 * @note Owns and releases the sparse mapping reservation on every exit path.
 */
int main(void) {
    int result = 0;
    void *mapping = reserve_mapping();
    CheckMain(mapping != NULL);
    CheckMain(((uintptr_t)mapping % InteropShmAlignment) == 0);

    interop_shm_header_t *header = initialize_mapping(mapping);
    interop_slot_descriptor_t *descriptors =
        (interop_slot_descriptor_t *)((uint8_t *)mapping + InteropNativeDescriptorOffset);
    for (uint32_t index = 0; index < InteropNativeDescriptorCount; ++index) {
        atomic_init(&descriptors[index].state, InteropSlotFree);
    }
    interop_slot_descriptor_t *descriptor = &descriptors[0];
    CheckMain(interop_atomic_validate_mapping((uintptr_t)mapping, InteropNativeMappingBytes,
        header, descriptor) == InteropAtomicStatusOk);
    CheckMain(test_invalid_arguments(mapping, header, descriptor) == 0);
    CheckMain(test_transition_matrix(descriptor, (uintptr_t)mapping) == 0);
    CheckMain(test_heartbeats(header, (uintptr_t)mapping) == 0);
    atomic_store_explicit(&header->guest_heartbeat, 0, memory_order_relaxed);
    CheckMain(test_stress(mapping, descriptor) == 0);

cleanup:
    release_mapping(mapping);
    return result;
}
