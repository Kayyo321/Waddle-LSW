/**
 * @file window_coordinator_faults.h
 * @brief Fault injection at the ABI seams the window coordinator calls.
 *
 * The suite links with -Wl,--wrap for each symbol listed in tests/interop/window_coordinator_wraps.txt. Every
 * __wrap_ function fails its next N calls, where N is the matching counter in Faults, and otherwise
 * forwards to the real implementation through __real_. The real Zig ABI objects, the real observer
 * and the real adapter are therefore exercised on every call that is not injected.
 *
 * Threading: the counters are plain data. The suite changes them on the owner thread only, and the
 * coordinator calls the wrappers on the owner thread only.
 * Ownership: the wrappers never allocate; a failed call leaves its outputs as the real call would on
 * the same failure.
 */
#ifndef WADDLE_TESTS_WINDOW_COORDINATOR_FAULTS_H
#define WADDLE_TESTS_WINDOW_COORDINATOR_FAULTS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "interop_slot_runtime.h"
#include "window_observer_windows.h"
#include "window_state.h"
#include "window_state_adapter_windows.h"
#include "waddle/interop_window_graph.h"

/* Every symbol below is listed in tests/interop/window_coordinator_wraps.txt, which the link reads. */

/** @brief Calls to fail next, per seam. Zero passes through. */
typedef struct coordinator_faults_t {
    uint32_t slot_create;
    uint32_t slot_admit;
    uint32_t slot_admit_heap;
    uint32_t slot_prepare;
    uint32_t slot_writer;
    uint32_t slot_commit;
    uint32_t slot_cancel_unentered;
    uint32_t slot_release;
    uint32_t slot_retire;
    uint32_t slot_begin_drain;
    uint32_t slot_confirm;
    uint32_t graph_create;
    uint32_t graph_complete;
    uint32_t graph_invalidate;
    uint32_t graph_next_event;
    uint32_t observer_create;
    uint32_t observer_bind;
    uint32_t observer_start;
    uint32_t observer_sync;
    uint32_t observer_destroy;
    uint32_t adapter_create;
    uint32_t adapter_admit;
    uint32_t adapter_observe;
    uint32_t adapter_execute;
    uint32_t adapter_release;
    uint32_t reducer_create;
    uint32_t reducer_plan;
    uint32_t reducer_complete;
    uint32_t observer_stop;
    uint32_t allocation;
    uint32_t writer_finish;
    uint32_t reducer_unsynchronized;
    uint32_t observer_drain;
} coordinator_faults_t;

/** @brief The one fault plan of the suite. fixture_open clears it. */
static coordinator_faults_t Faults;

/** @brief Consumes one injected failure from a counter. Returns 1 when the call must fail. */
static int fault_take(uint32_t *counter) {
    if (*counter == 0u) return 0;
    *counter -= 1u;
    return 1;
}

extern waddle_slot_status_t __real_waddle_slot_runtime_create(uint64_t session_epoch,
                                                              waddle_slot_runtime_handle_t **out_handle);
waddle_slot_status_t __wrap_waddle_slot_runtime_create(uint64_t session_epoch, waddle_slot_runtime_handle_t **out_handle) {
    if (fault_take(&Faults.slot_create)) return WaddleSlotOutOfMemory;
    return __real_waddle_slot_runtime_create(session_epoch, out_handle);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_admit_window(waddle_slot_runtime_handle_t *handle,
                                                                    uint64_t session_epoch, uint64_t window,
                                                                    uint64_t generation, uint32_t width, uint32_t height,
                                                                    waddle_slot_view_t out_views[3]);
waddle_slot_status_t __wrap_waddle_slot_runtime_admit_window(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                             uint64_t window, uint64_t generation, uint32_t width,
                                                             uint32_t height, waddle_slot_view_t out_views[3]) {
    if (fault_take(&Faults.slot_admit_heap)) return WaddleSlotHeapFull;
    if (fault_take(&Faults.slot_admit)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_admit_window(handle, session_epoch, window, generation, width, height, out_views);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_prepare_publish(waddle_slot_runtime_handle_t *handle,
                                                                       uint64_t session_epoch, uint64_t window,
                                                                       uint64_t generation, uint8_t slot,
                                                                       uint64_t frame_sequence,
                                                                       waddle_slot_view_t *out_view);
waddle_slot_status_t __wrap_waddle_slot_runtime_prepare_publish(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                                uint64_t window, uint64_t generation, uint8_t slot,
                                                                uint64_t frame_sequence, waddle_slot_view_t *out_view) {
    if (fault_take(&Faults.slot_prepare)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_prepare_publish(handle, session_epoch, window, generation, slot, frame_sequence, out_view);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_mark_writer_entered(waddle_slot_runtime_handle_t *handle,
                                                                           uint64_t session_epoch, uint64_t window,
                                                                           uint64_t generation, uint8_t slot,
                                                                           uint64_t frame_sequence);
waddle_slot_status_t __wrap_waddle_slot_runtime_mark_writer_entered(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                                    uint64_t window, uint64_t generation, uint8_t slot,
                                                                    uint64_t frame_sequence) {
    if (fault_take(&Faults.slot_writer)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_mark_writer_entered(handle, session_epoch, window, generation, slot, frame_sequence);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_commit_publish(waddle_slot_runtime_handle_t *handle,
                                                                      uint64_t session_epoch, uint64_t window,
                                                                      uint64_t generation, uint8_t slot,
                                                                      uint64_t frame_sequence);
waddle_slot_status_t __wrap_waddle_slot_runtime_commit_publish(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                               uint64_t window, uint64_t generation, uint8_t slot,
                                                               uint64_t frame_sequence) {
    if (fault_take(&Faults.slot_commit)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_commit_publish(handle, session_epoch, window, generation, slot, frame_sequence);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_cancel_unentered(waddle_slot_runtime_handle_t *handle,
                                                                        uint64_t session_epoch, uint64_t window,
                                                                        uint64_t generation, uint8_t slot,
                                                                        uint64_t frame_sequence);
waddle_slot_status_t __wrap_waddle_slot_runtime_cancel_unentered(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                                 uint64_t window, uint64_t generation, uint8_t slot,
                                                                 uint64_t frame_sequence) {
    if (fault_take(&Faults.slot_cancel_unentered)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_cancel_unentered(handle, session_epoch, window, generation, slot, frame_sequence);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_release_frame(waddle_slot_runtime_handle_t *handle,
                                                                     uint64_t session_epoch, uint64_t window,
                                                                     uint64_t generation, uint8_t slot,
                                                                     uint64_t frame_sequence);
waddle_slot_status_t __wrap_waddle_slot_runtime_release_frame(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                              uint64_t window, uint64_t generation, uint8_t slot,
                                                              uint64_t frame_sequence) {
    if (fault_take(&Faults.slot_release)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_release_frame(handle, session_epoch, window, generation, slot, frame_sequence);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_retire_window(waddle_slot_runtime_handle_t *handle,
                                                                     uint64_t session_epoch, uint64_t window,
                                                                     uint64_t generation);
waddle_slot_status_t __wrap_waddle_slot_runtime_retire_window(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                              uint64_t window, uint64_t generation) {
    if (fault_take(&Faults.slot_retire)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_retire_window(handle, session_epoch, window, generation);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_begin_drain(waddle_slot_runtime_handle_t *handle,
                                                                   uint64_t session_epoch, uint32_t producers_stopped);
waddle_slot_status_t __wrap_waddle_slot_runtime_begin_drain(waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
                                                            uint32_t producers_stopped) {
    if (fault_take(&Faults.slot_begin_drain)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_begin_drain(handle, session_epoch, producers_stopped);
}

extern waddle_slot_status_t __real_waddle_slot_runtime_confirm_host_drained(waddle_slot_runtime_handle_t *handle,
                                                                            uint64_t session_epoch,
                                                                            uint32_t host_buffers_destroyed);
waddle_slot_status_t __wrap_waddle_slot_runtime_confirm_host_drained(waddle_slot_runtime_handle_t *handle,
                                                                     uint64_t session_epoch, uint32_t host_buffers_destroyed) {
    if (fault_take(&Faults.slot_confirm)) return WaddleSlotInvalidArgument;
    return __real_waddle_slot_runtime_confirm_host_drained(handle, session_epoch, host_buffers_destroyed);
}

extern interop_window_graph_status_t __real_interop_window_graph_create(const interop_window_graph_config_t *config,
                                                                        interop_window_graph_t **out_graph);
interop_window_graph_status_t __wrap_interop_window_graph_create(const interop_window_graph_config_t *config,
                                                                 interop_window_graph_t **out_graph) {
    if (fault_take(&Faults.graph_create)) return InteropWindowGraphStatusNoMemory;
    return __real_interop_window_graph_create(config, out_graph);
}

extern interop_window_graph_status_t __real_interop_window_graph_complete_retire(interop_window_graph_t *graph,
                                                                                 uint64_t session_epoch, uint64_t window,
                                                                                 uint64_t generation,
                                                                                 const interop_window_retire_proof_t *proof);
interop_window_graph_status_t __wrap_interop_window_graph_complete_retire(interop_window_graph_t *graph, uint64_t session_epoch,
                                                                          uint64_t window, uint64_t generation,
                                                                          const interop_window_retire_proof_t *proof) {
    if (fault_take(&Faults.graph_complete)) return InteropWindowGraphStatusInvalidArgument;
    return __real_interop_window_graph_complete_retire(graph, session_epoch, window, generation, proof);
}

extern interop_window_graph_status_t __real_interop_window_graph_invalidate_all(interop_window_graph_t *graph,
                                                                                uint64_t session_epoch,
                                                                                uint32_t *retired_out);
interop_window_graph_status_t __wrap_interop_window_graph_invalidate_all(interop_window_graph_t *graph, uint64_t session_epoch,
                                                                         uint32_t *retired_out) {
    if (fault_take(&Faults.graph_invalidate)) return InteropWindowGraphStatusQueueFull;
    return __real_interop_window_graph_invalidate_all(graph, session_epoch, retired_out);
}

extern interop_window_graph_status_t __real_interop_window_graph_next_event(interop_window_graph_t *graph,
                                                                            uint64_t session_epoch,
                                                                            interop_window_event_t *event);
interop_window_graph_status_t __wrap_interop_window_graph_next_event(interop_window_graph_t *graph, uint64_t session_epoch,
                                                                     interop_window_event_t *event) {
    if (fault_take(&Faults.graph_next_event)) return InteropWindowGraphStatusInvalidArgument;
    return __real_interop_window_graph_next_event(graph, session_epoch, event);
}

extern window_observer_status_t __real_window_observer_create(const window_observer_config_t *config,
                                                              window_observer_t **out_observer);
window_observer_status_t __wrap_window_observer_create(const window_observer_config_t *config, window_observer_t **out_observer) {
    if (fault_take(&Faults.observer_create)) return WindowObserverStatusNoMemory;
    return __real_window_observer_create(config, out_observer);
}

extern window_observer_status_t __real_window_observer_bind_graph(window_observer_t *observer, interop_window_graph_t *graph,
                                                                  uint64_t session_epoch);
window_observer_status_t __wrap_window_observer_bind_graph(window_observer_t *observer, interop_window_graph_t *graph,
                                                           uint64_t session_epoch) {
    if (fault_take(&Faults.observer_bind)) return WindowObserverStatusBadState;
    return __real_window_observer_bind_graph(observer, graph, session_epoch);
}

extern window_observer_status_t __real_window_observer_start(window_observer_t *observer);
window_observer_status_t __wrap_window_observer_start(window_observer_t *observer) {
    if (fault_take(&Faults.observer_start)) return WindowObserverStatusHookFailed;
    return __real_window_observer_start(observer);
}

extern window_observer_status_t __real_window_observer_sync_tracked(window_observer_t *observer);
window_observer_status_t __wrap_window_observer_sync_tracked(window_observer_t *observer) {
    if (fault_take(&Faults.observer_sync)) return WindowObserverStatusGraphError;
    return __real_window_observer_sync_tracked(observer);
}

extern window_observer_status_t __real_window_observer_destroy(window_observer_t **observer_slot);
window_observer_status_t __wrap_window_observer_destroy(window_observer_t **observer_slot) {
    if (fault_take(&Faults.observer_destroy)) return WindowObserverStatusProtectedStatePending;
    return __real_window_observer_destroy(observer_slot);
}

extern window_state_adapter_status_t __real_window_state_adapter_create(const window_state_native_api_t *api, void *context,
                                                                        window_state_adapter_t **out_adapter);
window_state_adapter_status_t __wrap_window_state_adapter_create(const window_state_native_api_t *api, void *context,
                                                                 window_state_adapter_t **out_adapter) {
    if (fault_take(&Faults.adapter_create)) return WindowStateAdapterOutOfMemory;
    return __real_window_state_adapter_create(api, context, out_adapter);
}

extern window_state_adapter_status_t __real_window_state_adapter_admit(window_state_adapter_t *adapter, uintptr_t window,
                                                                       uint64_t window_id, uint32_t generation);
window_state_adapter_status_t __wrap_window_state_adapter_admit(window_state_adapter_t *adapter, uintptr_t window,
                                                                uint64_t window_id, uint32_t generation) {
    if (fault_take(&Faults.adapter_admit)) return WindowStateAdapterCapacityExhausted;
    return __real_window_state_adapter_admit(adapter, window, window_id, generation);
}

extern window_state_adapter_status_t __real_window_state_adapter_observe(window_state_adapter_t *adapter, uintptr_t window,
                                                                         uint64_t window_id, uint32_t generation,
                                                                         uint32_t *out_guest_state);
window_state_adapter_status_t __wrap_window_state_adapter_observe(window_state_adapter_t *adapter, uintptr_t window,
                                                                  uint64_t window_id, uint32_t generation,
                                                                  uint32_t *out_guest_state) {
    if (fault_take(&Faults.adapter_observe)) return WindowStateAdapterInvalidArgument;
    return __real_window_state_adapter_observe(adapter, window, window_id, generation, out_guest_state);
}

extern window_state_adapter_status_t __real_window_state_adapter_execute(window_state_adapter_t *adapter, uintptr_t window,
                                                                         uint64_t window_id, uint32_t generation,
                                                                         const waddle_window_state_action_t *actions,
                                                                         uint64_t count, uint64_t *out_completed);
window_state_adapter_status_t __wrap_window_state_adapter_execute(window_state_adapter_t *adapter, uintptr_t window,
                                                                  uint64_t window_id, uint32_t generation,
                                                                  const waddle_window_state_action_t *actions, uint64_t count,
                                                                  uint64_t *out_completed) {
    if (fault_take(&Faults.adapter_execute)) return WindowStateAdapterInvalidArgument;
    return __real_window_state_adapter_execute(adapter, window, window_id, generation, actions, count, out_completed);
}

extern window_state_adapter_status_t __real_window_state_adapter_release(window_state_adapter_t *adapter, uint64_t window_id,
                                                                         uint32_t generation);
window_state_adapter_status_t __wrap_window_state_adapter_release(window_state_adapter_t *adapter, uint64_t window_id,
                                                                  uint32_t generation) {
    if (fault_take(&Faults.adapter_release)) return WindowStateAdapterInvalidArgument;
    return __real_window_state_adapter_release(adapter, window_id, generation);
}

extern waddle_window_state_status_t __real_waddle_window_state_create(uint64_t window_id, uint32_t generation,
                                                                      uint32_t guest_state,
                                                                      waddle_window_state_handle_t **out_handle);
waddle_window_state_status_t __wrap_waddle_window_state_create(uint64_t window_id, uint32_t generation, uint32_t guest_state,
                                                               waddle_window_state_handle_t **out_handle) {
    if (fault_take(&Faults.reducer_create)) return WaddleWindowStateOutOfMemory;
    return __real_waddle_window_state_create(window_id, generation, guest_state, out_handle);
}

extern waddle_window_state_status_t __real_waddle_window_state_plan(const waddle_window_state_handle_t *handle,
                                                                    uint64_t window_id, uint32_t generation,
                                                                    const waddle_window_state_input_t *input,
                                                                    waddle_window_state_plan_t *out_plan);
waddle_window_state_status_t __wrap_waddle_window_state_plan(const waddle_window_state_handle_t *handle, uint64_t window_id,
                                                             uint32_t generation, const waddle_window_state_input_t *input,
                                                             waddle_window_state_plan_t *out_plan) {
    if (fault_take(&Faults.reducer_unsynchronized)) return WaddleWindowStateUnsynchronized;
    if (fault_take(&Faults.reducer_plan)) return WaddleWindowStateInvalidState;
    return __real_waddle_window_state_plan(handle, window_id, generation, input, out_plan);
}

extern waddle_window_state_status_t __real_waddle_window_state_complete(waddle_window_state_handle_t *handle,
                                                                        const waddle_window_state_plan_t *plan,
                                                                        uint32_t succeeded, uint64_t completed_actions);
waddle_window_state_status_t __wrap_waddle_window_state_complete(waddle_window_state_handle_t *handle,
                                                                 const waddle_window_state_plan_t *plan, uint32_t succeeded,
                                                                 uint64_t completed_actions) {
    if (fault_take(&Faults.reducer_complete)) return WaddleWindowStateInvalidState;
    return __real_waddle_window_state_complete(handle, plan, succeeded, completed_actions);
}

extern window_observer_status_t __real_window_observer_stop(window_observer_t *observer);
window_observer_status_t __wrap_window_observer_stop(window_observer_t *observer) {
    /* 1 refuses with GraphError without stopping; 2 stops for real and reports BadState (already stopped). */
    if (Faults.observer_stop != 0u) {
        uint32_t mode = Faults.observer_stop;
        Faults.observer_stop = 0u;
        if (mode == 2u) {
            (void)__real_window_observer_stop(observer);
            return WindowObserverStatusBadState;
        }
        return WindowObserverStatusGraphError;
    }
    return __real_window_observer_stop(observer);
}

extern void *__real_calloc(size_t count, size_t size);
/* Every calloc of the link passes through here; only the coordinator's own allocation is the first one counted in create. */
void *__wrap_calloc(size_t count, size_t size) {
    if (fault_take(&Faults.allocation)) return NULL;
    return __real_calloc(count, size);
}

extern int __real_interop_writer_finish(interop_writer_t *writer, uint16_t type, uint32_t request_id,
                                        interop_direction_t direction, size_t *frame_length);
int __wrap_interop_writer_finish(interop_writer_t *writer, uint16_t type, uint32_t request_id, interop_direction_t direction,
                                 size_t *frame_length) {
    if (fault_take(&Faults.writer_finish)) return InteropErrorProtocolViolation;
    return __real_interop_writer_finish(writer, type, request_id, direction, frame_length);
}

extern window_observer_status_t __real_window_observer_drain_protected_destroys(window_observer_t *observer);
window_observer_status_t __wrap_window_observer_drain_protected_destroys(window_observer_t *observer) {
    if (fault_take(&Faults.observer_drain)) return WindowObserverStatusGraphQueueFull;
    return __real_window_observer_drain_protected_destroys(observer);
}

#endif
