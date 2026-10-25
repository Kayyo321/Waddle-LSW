/**
 * @file window_coordinator.c
 * @brief Guest window runtime coordinator (TODO #10 C1-C3, #13 guest half). Contract: window_coordinator_contract.md.
 *
 * Portable: no Win32 call is made here. Every native operation goes through the window-state native table,
 * the observer table, the capture seam or the io seam that the caller supplies.
 */
#include "window_coordinator.h"

#include <stdlib.h>
#include <string.h>

/** @brief Slots per window (the three descriptors of one admitted generation). */
#define CoordSlotsPerWindow 3u

/* Entry kinds. */
enum {
    CoordEntryAnnounced = 1,
    CoordEntryRetiring = 2,
    CoordEntryPhantom = 3
};

/* Dispatch results of one staged event. Hold waits benignly (a barrier or a full FIFO); Retry is a
 * resource failure that may clear, and only consecutive retries of one event count toward a fault. */
enum {
    CoordEventDone = 0,
    CoordEventHold = 1,
    CoordEventRetry = 2
};

/** @brief One admitted generation the coordinator holds (exactly one graph entry per table entry). */
typedef struct coord_entry_t {
    /** @brief Nonzero while the entry is in use. */
    uint32_t in_use;
    /** @brief CoordEntryAnnounced, CoordEntryRetiring or CoordEntryPhantom. */
    uint32_t kind;
    /** @brief HWND value. */
    uint64_t window;
    /** @brief Graph generation. */
    uint64_t generation;
    /** @brief Parent HWND at admission, 0 when none. */
    uint64_t parent_window;
    /** @brief Parent generation at admission. */
    uint64_t parent_generation;
    /** @brief Role of the generation. */
    uint32_t role;
    /** @brief Last announced or updated geometry and derived state. */
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t state;
    uint32_t dpi;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t process_id;
    /** @brief Application key copy, valid bytes app_key_length. */
    uint8_t app_key[InteropMaxAppKeyBytes];
    uint32_t app_key_length;
    /** @brief Title copy, valid bytes title_length. */
    uint8_t title[InteropMaxTitleBytes];
    uint32_t title_length;
    /** @brief Nonzero while capture is started for this generation. */
    uint32_t capture_started;
    /** @brief Last frame sequence consumed for this generation (0 before the first). */
    uint32_t retire_sent; /**< 1 once WindowRetire was queued for this generation (IMPL 5.3: only after the drain). */
    uint64_t last_sequence;
    /** @brief Per slot: the published sequence still pinned by the host, or 0. */
    uint64_t pinned[CoordSlotsPerWindow];
    /** @brief Reducer handle for (window, generation), or NULL. */
    waddle_window_state_handle_t *reducer;
} coord_entry_t;

/** @brief One encoded outbound frame in the FIFO. */
typedef struct coord_frame_t {
    uint32_t length;
    uint8_t bytes[WaddleCoordinatorMaxFrameBytes];
} coord_frame_t;

/** @brief Opaque coordinator object. */
struct window_coordinator_t {
    /** @brief Configuration copied at create. */
    window_coordinator_config_t config;
    /** @brief Owner thread id from the observer clock table. */
    uint32_t owner_thread;
    /** @brief Nonzero while a coordinator entry is active on the owner thread. */
    uint32_t busy;
    /** @brief window_coordinator_state_t. */
    uint32_t state;
    /** @brief Epoch of the current graph, observer and slot runtime. */
    uint64_t epoch;
    /** @brief Current graph. */
    interop_window_graph_t *graph;
    /** @brief Current observer, bound to graph. */
    window_observer_t *observer;
    /** @brief Nonzero when the observer is started. */
    uint32_t observer_started;
    /** @brief Current slot runtime. */
    waddle_slot_runtime_handle_t *slots;
    /** @brief Nonzero once the slot runtime entered Draining in the Lost state. */
    uint32_t slots_draining;
    /** @brief Window-state adapter (one for the coordinator). */
    window_state_adapter_t *adapter;
    /** @brief Nonzero when invalidate_all must still be called (its ring room was missing). */
    uint32_t invalidate_pending;
    /** @brief Failed attempts of the head staged event. */
    uint32_t head_retries;
    /** @brief Staging ring of graph events. */
    uint32_t staging_head;
    uint32_t staging_count;
    /** @brief Outbound FIFO. */
    uint32_t outbound_head;
    uint32_t outbound_count;
    /** @brief Counters. */
    window_coordinator_stats_t stats;
    /** @brief Admitted generations. */
    coord_entry_t entries[InteropMaxWindows];
    /** @brief Staged graph events, oldest at staging_head. */
    interop_window_event_t staging[WaddleCoordinatorStagingCapacity];
    /** @brief Outbound frames, oldest at outbound_head. */
    coord_frame_t outbound[WaddleCoordinatorOutboundCapacity];
    /** @brief Receive buffer for one host frame. */
    uint8_t inbound[InteropMaxFrameBytes];
};

static void enter_lost(window_coordinator_t *coordinator);
static void retire_all_entries(window_coordinator_t *coordinator);
static int complete_graph_retirements(window_coordinator_t *coordinator);
static void drain_observer_destroys(window_coordinator_t *coordinator);

/* ---- Small helpers ----------------------------------------------------------------------------- */

static void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint64_t saturate_add(uint64_t value) {
    return value == UINT64_MAX ? value : value + 1u;
}

/** @brief Owner-thread and reentrancy gate. Returns Ok when the entry may proceed (busy is then set). */
static window_coordinator_status_t enter(window_coordinator_t *coordinator) {
    if (coordinator->config.observer_api->current_thread_id(coordinator->config.observer_api_context) !=
        coordinator->owner_thread) {
        return WindowCoordinatorWrongThread;
    }
    if (coordinator->busy != 0u) return WindowCoordinatorReentrant;
    coordinator->busy = 1u;
    return WindowCoordinatorOk;
}

static void leave(window_coordinator_t *coordinator) {
    coordinator->busy = 0u;
}

static uint32_t graph_ok(interop_window_graph_status_t status) {
    return status == InteropWindowGraphStatusOk ? 1u : 0u;
}

static coord_entry_t *find_pair(window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        coord_entry_t *entry = &coordinator->entries[index];
        if (entry->in_use != 0u && entry->window == window && entry->generation == generation) return entry;
    }
    return NULL;
}

static coord_entry_t *find_free(window_coordinator_t *coordinator) {
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        if (coordinator->entries[index].in_use == 0u) return &coordinator->entries[index];
    }
    return NULL;
}

static uint32_t entry_count(const window_coordinator_t *coordinator) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        if (coordinator->entries[index].in_use != 0u) count += 1u;
    }
    return count;
}

/** @brief True while any generation is quarantined; admissions are held until none is. */
static uint32_t retire_barrier_pending(const window_coordinator_t *coordinator) {
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        if (coordinator->entries[index].in_use != 0u && coordinator->entries[index].kind == CoordEntryRetiring) {
            return 1u;
        }
    }
    return 0u;
}

/** @brief True when an entry with this parent exists, so the parent may not complete its retirement yet. */
static uint32_t has_child(const window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        const coord_entry_t *entry = &coordinator->entries[index];
        if (entry->in_use != 0u && entry->parent_window == window && entry->parent_generation == generation) {
            return 1u;
        }
    }
    return 0u;
}

static uint32_t pins_held(const coord_entry_t *entry) {
    for (uint32_t slot = 0; slot < CoordSlotsPerWindow; ++slot) {
        if (entry->pinned[slot] != 0u) return 1u;
    }
    return 0u;
}

/* ---- Outbound FIFO and frame encoders ---------------------------------------------------------- */

static int outbound_full(const window_coordinator_t *coordinator) {
    return coordinator->outbound_count >= WaddleCoordinatorOutboundCapacity;
}

/** @brief Append an encoded frame. Returns 0 on success, 1 when the FIFO is full (nothing changed). */
static int outbound_push(window_coordinator_t *coordinator, const uint8_t *bytes, size_t length) {
    uint32_t tail;
    if (outbound_full(coordinator)) return 1;
    tail = (coordinator->outbound_head + coordinator->outbound_count) % WaddleCoordinatorOutboundCapacity;
    memcpy(coordinator->outbound[tail].bytes, bytes, length);
    coordinator->outbound[tail].length = (uint32_t)length;
    coordinator->outbound_count += 1u;
    return 0;
}

/** @brief Finish a writer and queue its frame. Returns 0 on success. */
static int finish_frame(window_coordinator_t *coordinator, interop_writer_t *writer, uint8_t *frame, uint16_t type) {
    size_t length = 0;
    if (writer->failed != 0) return 1;
    if (interop_writer_finish(writer, type, 0u, InteropFromGuest, &length) != InteropErrorNone) return 1;
    return outbound_push(coordinator, frame, length);
}

/* Fields are written in ascending tag order: the codec requires canonical order. */

/** @brief WindowAnnounce for one entry (required fields, then optional fields, all in ascending tag order). */
static int encode_announce(window_coordinator_t *coordinator, const coord_entry_t *entry) {
    uint8_t frame[WaddleCoordinatorMaxFrameBytes];
    interop_writer_t writer;
    uint8_t min_size[8];
    interop_writer_init(&writer, frame, sizeof(frame));
    (void)interop_put_uint(&writer, InteropTagWindow, entry->window);
    (void)interop_put_uint(&writer, InteropTagGeneration, entry->generation);
    if (entry->parent_window != 0u) {
        (void)interop_put_uint(&writer, InteropTagParent, entry->parent_window);
        (void)interop_put_uint(&writer, InteropTagParentGeneration, entry->parent_generation);
    }
    (void)interop_put_uint(&writer, InteropTagRole, entry->role);
    (void)interop_put_rect(&writer, InteropTagRect, entry->x, entry->y, entry->width, entry->height);
    if (entry->title_length != 0u) {
        (void)interop_put_bytes(&writer, InteropTagTitle, entry->title, (uint16_t)entry->title_length);
    }
    (void)interop_put_bytes(&writer, InteropTagAppKey, entry->app_key, (uint16_t)entry->app_key_length);
    if (entry->process_id != 0u) {
        (void)interop_put_uint(&writer, InteropTagProcessId, entry->process_id);
    }
    (void)interop_put_uint(&writer, InteropTagState, entry->state);
    (void)interop_put_uint(&writer, InteropTagDpi, entry->dpi);
    if (entry->min_width != 0u || entry->min_height != 0u) {
        put_le32(min_size, entry->min_width);
        put_le32(min_size + 4, entry->min_height);
        (void)interop_put_bytes(&writer, InteropTagMinSize, min_size, 8);
    }
    return finish_frame(coordinator, &writer, frame, InteropMsgWindowAnnounce);
}

/** @brief WindowUpdate: window, generation, rect, optional title, state, dpi, optional minimum size. */
static int encode_update(window_coordinator_t *coordinator, const coord_entry_t *entry, uint32_t changed_title) {
    uint8_t frame[WaddleCoordinatorMaxFrameBytes];
    interop_writer_t writer;
    uint8_t min_size[8];
    interop_writer_init(&writer, frame, sizeof(frame));
    (void)interop_put_uint(&writer, InteropTagWindow, entry->window);
    (void)interop_put_uint(&writer, InteropTagGeneration, entry->generation);
    (void)interop_put_rect(&writer, InteropTagRect, entry->x, entry->y, entry->width, entry->height);
    if (changed_title != 0u) {
        (void)interop_put_bytes(&writer, InteropTagTitle, entry->title, (uint16_t)entry->title_length);
    }
    (void)interop_put_uint(&writer, InteropTagState, entry->state);
    (void)interop_put_uint(&writer, InteropTagDpi, entry->dpi);
    if (entry->min_width != 0u || entry->min_height != 0u) {
        put_le32(min_size, entry->min_width);
        put_le32(min_size + 4, entry->min_height);
        (void)interop_put_bytes(&writer, InteropTagMinSize, min_size, 8);
    }
    return finish_frame(coordinator, &writer, frame, InteropMsgWindowUpdate);
}

static int encode_retire(window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    uint8_t frame[WaddleCoordinatorMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof(frame));
    (void)interop_put_uint(&writer, InteropTagWindow, window);
    (void)interop_put_uint(&writer, InteropTagGeneration, generation);
    return finish_frame(coordinator, &writer, frame, InteropMsgWindowRetire);
}

static int encode_frame_ready(window_coordinator_t *coordinator, uint64_t window, uint64_t generation, uint8_t slot,
                              uint64_t sequence) {
    uint8_t frame[WaddleCoordinatorMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof(frame));
    (void)interop_put_uint(&writer, InteropTagWindow, window);
    (void)interop_put_uint(&writer, InteropTagGeneration, generation);
    (void)interop_put_uint(&writer, InteropTagSlot, slot);
    (void)interop_put_uint(&writer, InteropTagFrameSequence, sequence);
    return finish_frame(coordinator, &writer, frame, InteropMsgFrameReady);
}

static int encode_stale_error(window_coordinator_t *coordinator) {
    uint8_t frame[WaddleCoordinatorMaxFrameBytes];
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof(frame));
    (void)interop_put_uint(&writer, InteropTagErrorCode, InteropErrorStaleWindow);
    return finish_frame(coordinator, &writer, frame, InteropMsgError);
}

/* ---- Native state teardown and reducer glue -------------------------------------------------- */

/** @brief Releases the adapter entry, leaving fullscreen first when the adapter refuses with Busy. */
static void release_native(window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    window_state_adapter_status_t status = window_state_adapter_release(coordinator->adapter, window, (uint32_t)generation);
    if (status == WindowStateAdapterBusy) {
        waddle_window_state_action_t leave;
        uint64_t completed = 0;
        memset(&leave, 0, sizeof(leave));
        leave.kind = WaddleWindowStateActionGuestLeaveFullscreen;
        (void)window_state_adapter_execute(coordinator->adapter, window, window, (uint32_t)generation, &leave, 1, &completed);
        (void)window_state_adapter_release(coordinator->adapter, window, (uint32_t)generation);
    }
}

/** @brief Reads the current guest state through the adapter, or fails. */
static int observe_native(window_coordinator_t *coordinator, uint64_t window, uint64_t generation, uint32_t *state) {
    return window_state_adapter_observe(coordinator->adapter, window, window, (uint32_t)generation, state) ==
           WindowStateAdapterOk ? 0 : 1;
}

/**
 * @brief Plans a GuestObservation from the native state and completes its host actions as applied.
 * Used after a native change the coordinator did not make (an Update) and to resynchronize after a
 * failed guest action.
 */
static void resync_reducer(window_coordinator_t *coordinator, coord_entry_t *entry) {
    uint32_t state = 0;
    waddle_window_state_input_t input;
    waddle_window_state_plan_t plan;
    if (entry->reducer == NULL) return;
    if (observe_native(coordinator, entry->window, entry->generation, &state) != 0) {
        coordinator->stats.reducer_failures = saturate_add(coordinator->stats.reducer_failures);
        return;
    }
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputGuestObservation;
    input.state = state;
    if (waddle_window_state_plan(entry->reducer, entry->window, (uint32_t)entry->generation, &input, &plan) !=
        WaddleWindowStateOk) {
        coordinator->stats.reducer_failures = saturate_add(coordinator->stats.reducer_failures);
        return;
    }
    if (waddle_window_state_complete(entry->reducer, &plan, 1u, plan.action_count) != WaddleWindowStateOk) {
        coordinator->stats.reducer_failures = saturate_add(coordinator->stats.reducer_failures);
    }
}

/**
 * @brief Plans one host input, executes its guest actions through the adapter, and completes the plan.
 * On a failed or partial execution the reducer is left unsynchronized and resynchronizes on the next input.
 */
static int run_reducer_input(window_coordinator_t *coordinator, coord_entry_t *entry,
                             const waddle_window_state_input_t *input) {
    waddle_window_state_plan_t plan;
    waddle_window_state_status_t plan_status;
    uint64_t completed = 0;
    window_state_adapter_status_t status;
    int ok;
    if (entry->reducer == NULL) return 1;
    /* One plan call per input. An unsynchronized reducer is resynchronized once and planned again. */
    plan_status = waddle_window_state_plan(entry->reducer, entry->window, (uint32_t)entry->generation, input, &plan);
    if (plan_status == WaddleWindowStateUnsynchronized) {
        resync_reducer(coordinator, entry);
        plan_status = waddle_window_state_plan(entry->reducer, entry->window, (uint32_t)entry->generation, input, &plan);
    }
    if (plan_status != WaddleWindowStateOk) {
        coordinator->stats.reducer_failures = saturate_add(coordinator->stats.reducer_failures);
        return 1;
    }
    status = window_state_adapter_execute(coordinator->adapter, entry->window, entry->window, (uint32_t)entry->generation,
                                          plan.actions, plan.action_count, &completed);
    ok = status == WindowStateAdapterOk;
    if (waddle_window_state_complete(entry->reducer, &plan, ok ? 1u : 0u, completed) != WaddleWindowStateOk) {
        coordinator->stats.reducer_failures = saturate_add(coordinator->stats.reducer_failures);
    }
    return ok ? 0 : 1;
}

/* ---- Capture and slot helpers ---------------------------------------------------------------- */

static void capture_stop(window_coordinator_t *coordinator, coord_entry_t *entry) {
    if (entry->capture_started != 0u) {
        coordinator->config.capture->stop(coordinator->config.capture->context, entry->window, (uint32_t)entry->generation);
        entry->capture_started = 0u;
    }
}

/** @brief Retires the slot descriptors of one generation (no new claims; freed when pins end). */
static void slots_retire(window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    (void)waddle_slot_runtime_retire_window(coordinator->slots, coordinator->epoch, window, generation);
}

/** @brief Undoes a partial admission: releases every resource taken for (window, generation). */
static void undo_admission(window_coordinator_t *coordinator, coord_entry_t *entry, int adapter_taken, int reducer_taken,
                           int slots_taken) {
    if (reducer_taken != 0 && entry->reducer != NULL) {
        waddle_window_state_destroy(&entry->reducer);
    }
    if (adapter_taken != 0) release_native(coordinator, entry->window, entry->generation);
    if (slots_taken != 0) slots_retire(coordinator, entry->window, entry->generation);
}

/* ---- Staged event dispatch --------------------------------------------------------------------- */

/** @brief Turns an entry into a phantom: a generation the graph announced that the host never sees. */
static void make_phantom(window_coordinator_t *coordinator, coord_entry_t *entry, const interop_window_event_t *event) {
    memset(entry, 0, sizeof(*entry));
    entry->in_use = 1u;
    entry->kind = CoordEntryPhantom;
    entry->window = event->window;
    entry->generation = event->generation;
    coordinator->stats.phantoms = saturate_add(coordinator->stats.phantoms);
}

/** @brief Admits an Announce event, or records it as a phantom when the window cannot be served. */
static int handle_announce(window_coordinator_t *coordinator, const interop_window_event_t *event) {
    coord_entry_t *entry;
    coord_entry_t *parent;
    uint32_t state = 0;
    waddle_slot_view_t views[CoordSlotsPerWindow];
    int adapter_taken = 0;
    int reducer_taken = 0;
    int slots_taken = 0;
    int phantom = 0;

    if (find_pair(coordinator, event->window, event->generation) != NULL) return CoordEventDone;
    if (retire_barrier_pending(coordinator) != 0u && coordinator->state == WindowCoordinatorStateRunning) {
        coordinator->stats.admissions_held = saturate_add(coordinator->stats.admissions_held);
        return CoordEventHold;
    }
    entry = find_free(coordinator);
    if (entry == NULL) return CoordEventHold;

    if (coordinator->state != WindowCoordinatorStateRunning) {
        phantom = 1;
    } else if (event->parent_window != 0u) {
        parent = find_pair(coordinator, event->parent_window, event->parent_generation);
        if (parent == NULL || parent->kind != CoordEntryAnnounced) phantom = 1;
    }
    if (phantom == 0 && outbound_full(coordinator)) {
        coordinator->stats.admissions_held = saturate_add(coordinator->stats.admissions_held);
        return CoordEventHold;
    }

    memset(entry, 0, sizeof(*entry));
    entry->in_use = 1u;
    entry->window = event->window;
    entry->generation = event->generation;
    entry->parent_window = event->parent_window;
    entry->parent_generation = event->parent_generation;
    entry->role = event->role;
    entry->x = event->x;
    entry->y = event->y;
    entry->width = event->width;
    entry->height = event->height;
    entry->state = event->state;
    entry->dpi = event->dpi;
    entry->min_width = event->min_width;
    entry->min_height = event->min_height;
    entry->process_id = event->process_id;
    entry->app_key_length = event->app_key_length;
    memcpy(entry->app_key, event->app_key, event->app_key_length);
    entry->title_length = event->title_length;
    memcpy(entry->title, event->title, event->title_length);
    entry->kind = CoordEntryPhantom;

    if (phantom != 0) {
        coordinator->stats.phantoms = saturate_add(coordinator->stats.phantoms);
        return CoordEventDone;
    }

    if (waddle_slot_runtime_admit_window(coordinator->slots, coordinator->epoch, event->window, event->generation,
                                         event->width, event->height, views) != WaddleSlotOk) {
        /* Heap or descriptor shortage is temporary: retry; a permanent shortage faults. */
        coordinator->stats.admission_failures = saturate_add(coordinator->stats.admission_failures);
        memset(entry, 0, sizeof(*entry));
        return CoordEventRetry;
    }
    slots_taken = 1;
    if (window_state_adapter_admit(coordinator->adapter, event->window, event->window, (uint32_t)event->generation) !=
        WindowStateAdapterOk) {
        coordinator->stats.admission_failures = saturate_add(coordinator->stats.admission_failures);
        undo_admission(coordinator, entry, adapter_taken, reducer_taken, slots_taken);
        make_phantom(coordinator, entry, event);
        return CoordEventDone;
    }
    adapter_taken = 1;
    if (observe_native(coordinator, event->window, event->generation, &state) != 0) {
        coordinator->stats.admission_failures = saturate_add(coordinator->stats.admission_failures);
        undo_admission(coordinator, entry, adapter_taken, reducer_taken, slots_taken);
        make_phantom(coordinator, entry, event);
        return CoordEventDone;
    }
    if (waddle_window_state_create(event->window, (uint32_t)event->generation, state, &entry->reducer) !=
        WaddleWindowStateOk) {
        coordinator->stats.admission_failures = saturate_add(coordinator->stats.admission_failures);
        undo_admission(coordinator, entry, adapter_taken, reducer_taken, slots_taken);
        make_phantom(coordinator, entry, event);
        return CoordEventDone;
    }
    reducer_taken = 1;
    resync_reducer(coordinator, entry);
    if (coordinator->config.capture->start(coordinator->config.capture->context, event->window,
                                           (uint32_t)event->generation, event->width, event->height) != 0) {
        coordinator->stats.capture_failures = saturate_add(coordinator->stats.capture_failures);
        undo_admission(coordinator, entry, adapter_taken, reducer_taken, slots_taken);
        make_phantom(coordinator, entry, event);
        return CoordEventDone;
    }
    entry->capture_started = 1u;
    if (encode_announce(coordinator, entry) != 0) {
        /* Cannot happen while the FIFO had room; checked above. Treated as a fault to stay honest. */
        coordinator->state = WindowCoordinatorStateFaulted;
        return CoordEventDone;
    }
    entry->kind = CoordEntryAnnounced;
    coordinator->stats.announces = saturate_add(coordinator->stats.announces);
    return CoordEventDone;
}

/** @brief Forwards an Update for an announced generation, then resynchronizes the reducer. */
static int handle_update(window_coordinator_t *coordinator, const interop_window_event_t *event) {
    coord_entry_t *entry = find_pair(coordinator, event->window, event->generation);
    uint32_t title_changed;
    if (entry == NULL || entry->kind != CoordEntryAnnounced) return CoordEventDone;
    if (coordinator->state != WindowCoordinatorStateRunning) return CoordEventDone;
    if (outbound_full(coordinator)) {
        coordinator->stats.admissions_held = saturate_add(coordinator->stats.admissions_held);
        return CoordEventHold;
    }
    title_changed = (event->changed_mask & InteropWindowChangeTitle) != 0u ? 1u : 0u;
    entry->x = event->x;
    entry->y = event->y;
    entry->width = event->width;
    entry->height = event->height;
    entry->state = event->state;
    entry->dpi = event->dpi;
    entry->min_width = event->min_width;
    entry->min_height = event->min_height;
    if (title_changed != 0u) {
        entry->title_length = event->title_length;
        memcpy(entry->title, event->title, event->title_length);
    }
    if (encode_update(coordinator, entry, title_changed) != 0) {
        coordinator->state = WindowCoordinatorStateFaulted;
        return CoordEventDone;
    }
    coordinator->stats.updates = saturate_add(coordinator->stats.updates);
    resync_reducer(coordinator, entry);
    return CoordEventDone;
}

/** @brief Completes the graph's retirement of a generation that has no coordinator state. */
static void complete_untracked(window_coordinator_t *coordinator, uint64_t window, uint64_t generation) {
    interop_window_retire_proof_t proof;
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    if (graph_ok(interop_window_graph_complete_retire(coordinator->graph, coordinator->epoch, window, generation,
                                                      &proof)) != 0u) {
        coordinator->stats.retire_completed = saturate_add(coordinator->stats.retire_completed);
    }
}

/**
 * @brief Quarantines an announced generation: stops its capture, retires its slots and native state, and
 *        sends WindowRetire when the session is up. Completion waits for the frame pins.
 */
static int handle_retire_intent(window_coordinator_t *coordinator, const interop_window_event_t *event) {
    coord_entry_t *entry = find_pair(coordinator, event->window, event->generation);
    if (entry == NULL) {
        complete_untracked(coordinator, event->window, event->generation);
        return CoordEventDone;
    }
    if (entry->kind == CoordEntryPhantom) {
        if (entry->reducer != NULL) waddle_window_state_destroy(&entry->reducer);
        memset(entry, 0, sizeof(*entry));
        complete_untracked(coordinator, event->window, event->generation);
        return CoordEventDone;
    }
    if (entry->kind != CoordEntryAnnounced) return CoordEventDone;
    capture_stop(coordinator, entry);
    slots_retire(coordinator, entry->window, entry->generation);
    if (entry->reducer != NULL) waddle_window_state_destroy(&entry->reducer);
    release_native(coordinator, entry->window, entry->generation);
    entry->kind = CoordEntryRetiring;
    return CoordEventDone;
}

/** @brief Routes one staged event. Lost state: announcements become phantoms and nothing is sent. */
static int dispatch_event(window_coordinator_t *coordinator, const interop_window_event_t *event) {
    switch (event->kind) {
    case InteropWindowEventAnnounce:
        return handle_announce(coordinator, event);
    case InteropWindowEventUpdate:
        return handle_update(coordinator, event);
    case InteropWindowEventRetireIntent:
        return handle_retire_intent(coordinator, event);
    default:
        return CoordEventDone;
    }
}

/** @brief Processes staged events in order until one must wait. */
static void process_staging(window_coordinator_t *coordinator) {
    while (coordinator->staging_count != 0u && coordinator->state != WindowCoordinatorStateFaulted) {
        const interop_window_event_t *head = &coordinator->staging[coordinator->staging_head];
        int result = dispatch_event(coordinator, head);
        if (result == CoordEventHold) break;
        if (result == CoordEventRetry) {
            coordinator->head_retries += 1u;
            if (coordinator->head_retries > WaddleCoordinatorMaxAdmissionRetries) {
                coordinator->state = WindowCoordinatorStateFaulted;
            }
            break;
        }
        coordinator->head_retries = 0u;
        coordinator->staging_head = (coordinator->staging_head + 1u) % WaddleCoordinatorStagingCapacity;
        coordinator->staging_count -= 1u;
    }
}

/** @brief Copies graph events into the staging ring while it has room. */
static void stage_graph_events(window_coordinator_t *coordinator) {
    interop_window_event_t event;
    while (coordinator->staging_count < WaddleCoordinatorStagingCapacity) {
        interop_window_graph_status_t status = interop_window_graph_next_event(coordinator->graph, coordinator->epoch, &event);
        if (status != InteropWindowGraphStatusOk) break;
        coordinator->staging[(coordinator->staging_head + coordinator->staging_count) % WaddleCoordinatorStagingCapacity] = event;
        coordinator->staging_count += 1u;
        coordinator->stats.events_staged = saturate_add(coordinator->stats.events_staged);
    }
}

/* ---- Retire completion ----------------------------------------------------------------------- */

/**
 * @brief Completes every quarantined generation whose pins are gone and whose children are completed.
 *        A parent is skipped while any child is still in the table, so passes repeat until a pass makes no
 *        progress: children complete first whatever the table order.
 */
static void progress_retirements(window_coordinator_t *coordinator) {
    for (uint32_t pass = 0; pass < InteropMaxWindows; ++pass) {
        int progressed = 0;
        for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
            coord_entry_t *entry = &coordinator->entries[index];
            interop_window_retire_proof_t proof;
            interop_window_graph_status_t status;
            if (entry->in_use == 0u || entry->kind != CoordEntryRetiring) continue;
            if (pins_held(entry) != 0u) continue;
            if (has_child(coordinator, entry->window, entry->generation) != 0u) continue;
            if (coordinator->state == WindowCoordinatorStateRunning && entry->retire_sent == 0u) {
                /* The FIFO-full case waits: a dropped Retire would leave the host holding a generation forever. */
                if (outbound_full(coordinator)) continue;
                if (encode_retire(coordinator, entry->window, entry->generation) != 0) {
                    coordinator->state = WindowCoordinatorStateFaulted;
                    return;
                }
                entry->retire_sent = 1u;
                coordinator->stats.retires = saturate_add(coordinator->stats.retires);
            }
            proof.capture_stopped = 1u;
            proof.pins_drained = 1u;
            status = interop_window_graph_complete_retire(coordinator->graph, coordinator->epoch, entry->window,
                                                          entry->generation, &proof);
            if (status == InteropWindowGraphStatusOk || status == InteropWindowGraphStatusStale) {
                memset(entry, 0, sizeof(*entry));
                if (status == InteropWindowGraphStatusOk) {
                    coordinator->stats.retire_completed = saturate_add(coordinator->stats.retire_completed);
                }
                (void)window_observer_sync_tracked(coordinator->observer);
                progressed = 1;
            } else {
                coordinator->stats.admission_failures = saturate_add(coordinator->stats.admission_failures);
            }
        }
        if (progressed == 0) break;
    }
}

/* ---- Frame publish --------------------------------------------------------------------------- */

/** @brief Publishes at most one frame for one announced generation through the slot runtime. */
static void publish_one(window_coordinator_t *coordinator, coord_entry_t *entry) {
    uint8_t slot = 0;
    uint64_t sequence;
    waddle_slot_view_t view;
    int found = 0;
    int write_result;
    for (uint8_t offset = 0; offset < CoordSlotsPerWindow && found == 0; ++offset) {
        uint8_t candidate = (uint8_t)((entry->last_sequence + offset) % CoordSlotsPerWindow);
        if (entry->pinned[candidate] == 0u) {
            slot = candidate;
            found = 1;
        }
    }
    if (found == 0 || outbound_full(coordinator)) {
        coordinator->stats.frames_skipped = saturate_add(coordinator->stats.frames_skipped);
        return;
    }
    sequence = entry->last_sequence + 1u;
    if (waddle_slot_runtime_prepare_publish(coordinator->slots, coordinator->epoch, entry->window, entry->generation, slot,
                                            sequence, &view) != WaddleSlotOk) {
        coordinator->stats.frames_skipped = saturate_add(coordinator->stats.frames_skipped);
        return;
    }
    write_result = coordinator->config.capture->write_frame(coordinator->config.capture->context, entry->window,
                                                            (uint32_t)entry->generation, slot, sequence, &view);
    entry->last_sequence = sequence;
    if (write_result == 0) {
        if (waddle_slot_runtime_mark_writer_entered(coordinator->slots, coordinator->epoch, entry->window, entry->generation,
                                                    slot, sequence) != WaddleSlotOk) {
            /* The slot stays prepared but unentered; releasing it keeps the runtime drainable. */
            (void)waddle_slot_runtime_cancel_unentered(coordinator->slots, coordinator->epoch, entry->window, entry->generation,
                                                       slot, sequence);
            coordinator->state = WindowCoordinatorStateFaulted;
            return;
        }
        if (waddle_slot_runtime_commit_publish(coordinator->slots, coordinator->epoch, entry->window, entry->generation,
                                               slot, sequence) != WaddleSlotOk) {
            /* A writer that entered and never committed would keep the runtime from draining. */
            (void)waddle_slot_runtime_cancel_entered(coordinator->slots, coordinator->epoch, entry->window, entry->generation,
                                                     slot, sequence);
            coordinator->state = WindowCoordinatorStateFaulted;
            return;
        }
        entry->pinned[slot] = sequence;
        if (encode_frame_ready(coordinator, entry->window, entry->generation, slot, sequence) != 0) {
            coordinator->state = WindowCoordinatorStateFaulted;
            return;
        }
        coordinator->stats.frames_published = saturate_add(coordinator->stats.frames_published);
        return;
    }
    coordinator->stats.capture_failures = saturate_add(coordinator->stats.capture_failures);
    if (write_result == 1) {
        (void)waddle_slot_runtime_mark_writer_entered(coordinator->slots, coordinator->epoch, entry->window,
                                                      entry->generation, slot, sequence);
        (void)waddle_slot_runtime_cancel_entered(coordinator->slots, coordinator->epoch, entry->window, entry->generation,
                                                 slot, sequence);
    } else {
        (void)waddle_slot_runtime_cancel_unentered(coordinator->slots, coordinator->epoch, entry->window,
                                                   entry->generation, slot, sequence);
    }
}

static void publish_frames(window_coordinator_t *coordinator) {
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        coord_entry_t *entry = &coordinator->entries[index];
        if (entry->in_use == 0u || entry->kind != CoordEntryAnnounced || entry->capture_started == 0u) continue;
        for (uint32_t frame = 0; frame < WaddleCoordinatorFramesPerTick; ++frame) {
            publish_one(coordinator, entry);
            if (coordinator->state != WindowCoordinatorStateRunning) return;
        }
    }
}

/* ---- Host frames ----------------------------------------------------------------------------- */

static int read_u64(const uint8_t *payload, size_t length, uint16_t tag, uint64_t *value) {
    return interop_field_uint(payload, length, tag, 8u, value) == 0 ? 0 : 1;
}

static int read_u32(const uint8_t *payload, size_t length, uint16_t tag, uint32_t *value) {
    uint64_t wide = 0;
    if (interop_field_uint(payload, length, tag, 4u, &wide) != 0) return 1;
    *value = (uint32_t)wide;
    return 0;
}

/** @brief Replies Error{StaleWindow} to a host frame that names no exported generation. */
/**
 * @brief Counts a stale host frame and answers it with Error{StaleWindow}. A full outbound FIFO drops the answer:
 *        the host frame is already rejected, and a missing error reply is not a fault.
 */
static void reply_stale(window_coordinator_t *coordinator) {
    coordinator->stats.stale_replies = saturate_add(coordinator->stats.stale_replies);
    if (outbound_full(coordinator)) return;
    if (encode_stale_error(coordinator) != 0) coordinator->state = WindowCoordinatorStateFaulted;
}

static void apply_configure(window_coordinator_t *coordinator, coord_entry_t *entry, const uint8_t *payload, size_t length) {
    uint32_t state = 0;
    const uint8_t *rect = NULL;
    uint16_t rect_length = 0;
    waddle_window_state_input_t input;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    if (read_u32(payload, length, InteropTagState, &state) != 0) {
        coordinator->stats.host_frames_rejected = saturate_add(coordinator->stats.host_frames_rejected);
        return;
    }
    if (interop_field_get(payload, length, InteropTagRect, 0u, &rect, &rect_length) == 0 && rect_length == 16u) {
        x = (int32_t)((uint32_t)rect[0] | ((uint32_t)rect[1] << 8) | ((uint32_t)rect[2] << 16) | ((uint32_t)rect[3] << 24));
        y = (int32_t)((uint32_t)rect[4] | ((uint32_t)rect[5] << 8) | ((uint32_t)rect[6] << 16) | ((uint32_t)rect[7] << 24));
        width = (uint32_t)rect[8] | ((uint32_t)rect[9] << 8) | ((uint32_t)rect[10] << 16) | ((uint32_t)rect[11] << 24);
        height = (uint32_t)rect[12] | ((uint32_t)rect[13] << 8) | ((uint32_t)rect[14] << 16) | ((uint32_t)rect[15] << 24);
    }
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputHostConfigure;
    input.state = state;
    input.x = x;
    input.y = y;
    input.width = width;
    input.height = height;
    coordinator->stats.configures = saturate_add(coordinator->stats.configures);
    (void)run_reducer_input(coordinator, entry, &input);
}

static void apply_close(window_coordinator_t *coordinator, coord_entry_t *entry) {
    waddle_window_state_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = WaddleWindowStateInputCloseRequest;
    coordinator->stats.closes = saturate_add(coordinator->stats.closes);
    (void)run_reducer_input(coordinator, entry, &input);
}

/** @brief Handles one validated host frame. Only the Running state answers or acts. */
static void handle_host_frame(window_coordinator_t *coordinator, uint16_t type, const uint8_t *payload, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    coord_entry_t *entry;

    if (read_u64(payload, length, InteropTagWindow, &window) != 0 ||
        read_u64(payload, length, InteropTagGeneration, &generation) != 0) {
        coordinator->stats.host_frames_rejected = saturate_add(coordinator->stats.host_frames_rejected);
        return;
    }
    entry = find_pair(coordinator, window, generation);
    switch (type) {
    case InteropMsgFrameRelease: {
        uint32_t slot = 0;
        uint64_t sequence = 0;
        if (read_u32(payload, length, InteropTagSlot, &slot) != 0 ||
            read_u64(payload, length, InteropTagFrameSequence, &sequence) != 0 || slot >= CoordSlotsPerWindow) {
            coordinator->stats.host_frames_rejected = saturate_add(coordinator->stats.host_frames_rejected);
            return;
        }
        if (entry == NULL || (entry->kind != CoordEntryAnnounced && entry->kind != CoordEntryRetiring)) {
            reply_stale(coordinator);
            return;
        }
        if (entry->pinned[slot] != sequence ||
            waddle_slot_runtime_release_frame(coordinator->slots, coordinator->epoch, window, generation, (uint8_t)slot,
                                              sequence) != WaddleSlotOk) {
            coordinator->stats.releases_ignored = saturate_add(coordinator->stats.releases_ignored);
            return;
        }
        entry->pinned[slot] = 0u;
        coordinator->stats.releases_applied = saturate_add(coordinator->stats.releases_applied);
        return;
    }
    case InteropMsgWindowConfigure:
        if (entry == NULL || entry->kind != CoordEntryAnnounced) {
            reply_stale(coordinator);
            return;
        }
        apply_configure(coordinator, entry, payload, length);
        return;
    case InteropMsgWindowClose:
        if (entry == NULL || entry->kind != CoordEntryAnnounced) {
            reply_stale(coordinator);
            return;
        }
        apply_close(coordinator, entry);
        return;
    case InteropMsgPopupRepositioned:
    case InteropMsgPopupDismissed:
        if (entry == NULL || entry->kind != CoordEntryAnnounced) {
            reply_stale(coordinator);
            return;
        }
        coordinator->stats.popup_frames = saturate_add(coordinator->stats.popup_frames);
        return;
    default:
        coordinator->stats.host_frames_rejected = saturate_add(coordinator->stats.host_frames_rejected);
        return;
    }
}

/** @brief Receives and validates up to a bounded number of host frames. */
static void receive_host_frames(window_coordinator_t *coordinator) {
    for (uint32_t count = 0; count < 16u && coordinator->state == WindowCoordinatorStateRunning; ++count) {
        size_t length = 0;
        interop_header_t header;
        int received = coordinator->config.io->receive_frame(coordinator->config.io->context, coordinator->inbound,
                                                             sizeof(coordinator->inbound), &length);
        if (received < 0) {
            coordinator->stats.disconnects = saturate_add(coordinator->stats.disconnects);
            enter_lost(coordinator);
            return;
        }
        if (received == 0) return;
        if (interop_header_decode(coordinator->inbound, length, &header) != InteropErrorNone ||
            (size_t)InteropHeaderBytes + header.length != length ||
            interop_payload_validate(header.type, InteropFromHost, coordinator->inbound + InteropHeaderBytes,
                                     header.length) != InteropErrorNone) {
            coordinator->stats.host_frames_rejected = saturate_add(coordinator->stats.host_frames_rejected);
            continue;
        }
        handle_host_frame(coordinator, header.type, coordinator->inbound + InteropHeaderBytes, header.length);
    }
}

/* ---- Outbound flush -------------------------------------------------------------------------- */

static void flush_outbound(window_coordinator_t *coordinator) {
    while (coordinator->outbound_count != 0u && coordinator->state == WindowCoordinatorStateRunning) {
        coord_frame_t *head = &coordinator->outbound[coordinator->outbound_head];
        window_coordinator_send_result_t result =
            coordinator->config.io->send_frame(coordinator->config.io->context, head->bytes, head->length);
        if (result == WindowCoordinatorSent) {
            coordinator->outbound_head = (coordinator->outbound_head + 1u) % WaddleCoordinatorOutboundCapacity;
            coordinator->outbound_count -= 1u;
        } else if (result == WindowCoordinatorBackpressure) {
            coordinator->stats.backpressure = saturate_add(coordinator->stats.backpressure);
            return;
        } else {
            coordinator->stats.disconnects = saturate_add(coordinator->stats.disconnects);
            enter_lost(coordinator);
            return;
        }
    }
}

/* ---- Observer and graph drive ---------------------------------------------------------------- */

static void drive_observer(window_coordinator_t *coordinator, uint32_t budget) {
    uint64_t now;
    if (coordinator->observer_started == 0u) return;
    now = coordinator->config.observer_api->monotonic_ms(coordinator->config.observer_api_context);
    if (window_observer_refresh_due(coordinator->observer, now) != 0) {
        (void)window_observer_refresh(coordinator->observer, coordinator->graph, coordinator->epoch, NULL);
    }
    for (uint32_t round = 0; round < 4u; ++round) {
        window_observer_status_t status = window_observer_pump(coordinator->observer, coordinator->graph, coordinator->epoch,
                                                               budget, NULL);
        stage_graph_events(coordinator);
        process_staging(coordinator);
        if (status != WindowObserverStatusMore && status != WindowObserverStatusOk) break;
        if (status == WindowObserverStatusOk) break;
        if (coordinator->state != WindowCoordinatorStateRunning) break;
    }
}

/* ---- Lifecycle helpers ----------------------------------------------------------------------- */

static int start_observer(window_coordinator_t *coordinator) {
    if (window_observer_start(coordinator->observer) != WindowObserverStatusOk) return 1;
    coordinator->observer_started = 1u;
    return 0;
}

/** @brief Creates graph, observer and slot runtime for one epoch. Returns 0 on success. */
static int create_backends(window_coordinator_t *coordinator, uint64_t epoch) {
    interop_window_graph_config_t graph_config;
    window_observer_config_t observer_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = epoch;
    graph_config.uac_mode = coordinator->config.uac_mode;
    graph_config.app_key_lookup = coordinator->config.app_key_lookup;
    graph_config.app_key_context = coordinator->config.app_key_context;
    graph_config.now_ms = coordinator->config.now_ms;
    graph_config.now_ms_context = coordinator->config.now_ms_context;
    if (interop_window_graph_create(&graph_config, &coordinator->graph) != InteropWindowGraphStatusOk) return 1;
    coordinator->epoch = epoch;
    memset(&observer_config, 0, sizeof(observer_config));
    observer_config.api = coordinator->config.observer_api;
    observer_config.api_context = coordinator->config.observer_api_context;
    observer_config.first_stamp = 1u;
    observer_config.preedit_window = 0u;
    observer_config.own_process_id = coordinator->config.own_process_id;
    observer_config.uac_mode = coordinator->config.uac_mode;
    if (window_observer_create(&observer_config, &coordinator->observer) != WindowObserverStatusOk) return 1;
    if (window_observer_bind_graph(coordinator->observer, coordinator->graph, epoch) != WindowObserverStatusOk) return 1;
    if (waddle_slot_runtime_create(epoch, &coordinator->slots) != WaddleSlotOk) return 1;
    coordinator->slots_draining = 0u;
    coordinator->invalidate_pending = 0u;
    return 0;
}

static int has_windows_capability(const window_coordinator_t *coordinator) {
    return (coordinator->config.negotiated_capabilities & InteropCapWindows) != 0u;
}

static int config_valid(const window_coordinator_config_t *config) {
    if (config->session_epoch == 0u || config->uac_mode > InteropWindowUacNative) return 0;
    if (config->own_process_id == 0u || config->app_key_lookup == NULL) return 0;
    if (config->observer_api == NULL || config->native_api == NULL || config->io == NULL || config->capture == NULL) return 0;
    if (config->observer_api->current_thread_id == NULL || config->observer_api->monotonic_ms == NULL) return 0;
    if (config->io->send_frame == NULL || config->io->receive_frame == NULL) return 0;
    if (config->capture->start == NULL || config->capture->stop == NULL || config->capture->write_frame == NULL) return 0;
    return 1;
}

/* ---- Public API ------------------------------------------------------------------------------ */

/** @brief Frees a coordinator whose creation failed. No host ever saw it, so no drain proof is needed. */
static void free_unpublished(window_coordinator_t *coordinator) {
    if (coordinator->observer != NULL) {
        /* A bound observer must be stopped before it can be destroyed; BadState means it never started. */
        (void)window_observer_stop(coordinator->observer);
        (void)window_observer_destroy(&coordinator->observer);
    }
    if (coordinator->graph != NULL) (void)interop_window_graph_destroy(&coordinator->graph);
    if (coordinator->slots != NULL) {
        /* The caller's proof covers every host buffer of this epoch; a refused earlier drain step must not block the free. */
        (void)waddle_slot_runtime_begin_drain(coordinator->slots, coordinator->epoch, 1u);
        (void)waddle_slot_runtime_confirm_host_drained(coordinator->slots, coordinator->epoch, 1u);
        (void)waddle_slot_runtime_destroy(&coordinator->slots);
    }
    if (coordinator->adapter != NULL) (void)window_state_adapter_destroy(&coordinator->adapter);
    free(coordinator);
}

window_coordinator_status_t window_coordinator_create(const window_coordinator_config_t *config,
                                                      window_coordinator_t **out_coordinator) {
    window_coordinator_t *coordinator;
    if (out_coordinator == NULL) return WindowCoordinatorInvalidArgument;
    *out_coordinator = NULL;
    if (config == NULL || !config_valid(config)) return WindowCoordinatorInvalidArgument;
    coordinator = calloc(1u, sizeof(*coordinator));
    if (coordinator == NULL) return WindowCoordinatorOutOfMemory;
    coordinator->config = *config;
    coordinator->owner_thread = config->observer_api->current_thread_id(config->observer_api_context);
    coordinator->state = has_windows_capability(coordinator) ? WindowCoordinatorStateRunning : WindowCoordinatorStateIdle;
    coordinator->epoch = config->session_epoch;
    if (window_state_adapter_create(config->native_api, config->native_api_context, &coordinator->adapter) !=
        WindowStateAdapterOk) {
        free(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    coordinator->observer = NULL;
    coordinator->graph = NULL;
    coordinator->slots = NULL;
    if (create_backends(coordinator, config->session_epoch) != 0) {
        free_unpublished(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    if (has_windows_capability(coordinator) && start_observer(coordinator) != 0) {
        free_unpublished(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    *out_coordinator = coordinator;
    return WindowCoordinatorOk;
}

window_coordinator_status_t window_coordinator_destroy(window_coordinator_t **coordinator_slot, uint32_t host_buffers_destroyed) {
    window_coordinator_t *coordinator;
    window_coordinator_status_t status;
    if (coordinator_slot == NULL) return WindowCoordinatorInvalidArgument;
    coordinator = *coordinator_slot;
    if (coordinator == NULL) return WindowCoordinatorOk;
    status = enter(coordinator);
    if (status != WindowCoordinatorOk) return status;
    if (coordinator->state == WindowCoordinatorStateFaulted && host_buffers_destroyed == 1u) {
        /* A faulted coordinator makes no progress; the host proof lets its entries be ended here. */
        retire_all_entries(coordinator);
        (void)complete_graph_retirements(coordinator);
    }
    if (entry_count(coordinator) != 0u) {
        leave(coordinator);
        return WindowCoordinatorBusy;
    }
    if (coordinator->state != WindowCoordinatorStateIdle && host_buffers_destroyed != 1u) {
        leave(coordinator);
        return WindowCoordinatorMissingProof;
    }
    if (coordinator->observer != NULL) {
        /* A bound observer may leave Created or Stopped only through stop; BadState means it already is stopped. */
        window_observer_status_t stopped = window_observer_stop(coordinator->observer);
        if (stopped != WindowObserverStatusOk && stopped != WindowObserverStatusBadState) {
            leave(coordinator);
            return WindowCoordinatorBusy;
        }
        coordinator->observer_started = 0u;
        (void)window_observer_sync_tracked(coordinator->observer);
        /* Generations without an entry (an admission that was retried while its window went away) still listed by the graph would block the observer. */
        drain_observer_destroys(coordinator);
        (void)complete_graph_retirements(coordinator);
        if (window_observer_destroy(&coordinator->observer) != WindowObserverStatusOk) {
            leave(coordinator);
            return WindowCoordinatorBusy;
        }
    }
    if (coordinator->graph != NULL) {
        (void)interop_window_graph_destroy(&coordinator->graph);
    }
    if (coordinator->slots != NULL) {
        /* The caller's proof covers every host buffer of this epoch; a refused earlier drain step must not block the free. */
        (void)waddle_slot_runtime_begin_drain(coordinator->slots, coordinator->epoch, 1u);
        (void)waddle_slot_runtime_confirm_host_drained(coordinator->slots, coordinator->epoch, 1u);
        (void)waddle_slot_runtime_destroy(&coordinator->slots);
    }
    if (coordinator->adapter != NULL) {
        (void)window_state_adapter_destroy(&coordinator->adapter);
    }
    free(coordinator);
    *coordinator_slot = NULL;
    return WindowCoordinatorOk;
}

window_coordinator_status_t window_coordinator_tick(window_coordinator_t *coordinator, uint32_t budget,
                                                    window_coordinator_stats_t *stats_out) {
    window_coordinator_status_t status = WindowCoordinatorOk;
    if (coordinator == NULL || budget == 0u) return WindowCoordinatorInvalidArgument;
    status = enter(coordinator);
    if (status != WindowCoordinatorOk) return status;
    switch (coordinator->state) {
    case WindowCoordinatorStateIdle:
        status = WindowCoordinatorNoCapability;
        break;
    case WindowCoordinatorStateFaulted:
        status = WindowCoordinatorFaulted;
        break;
    case WindowCoordinatorStateLost:
        if (coordinator->invalidate_pending != 0u &&
            graph_ok(interop_window_graph_invalidate_all(coordinator->graph, coordinator->epoch, NULL)) != 0u) {
            coordinator->invalidate_pending = 0u;
        }
        stage_graph_events(coordinator);
        process_staging(coordinator);
        status = WindowCoordinatorLost;
        break;
    default:
        drive_observer(coordinator, budget);
        stage_graph_events(coordinator);
        process_staging(coordinator);
        if (coordinator->state == WindowCoordinatorStateRunning) receive_host_frames(coordinator);
        progress_retirements(coordinator);
        if (coordinator->state == WindowCoordinatorStateRunning) publish_frames(coordinator);
        flush_outbound(coordinator);
        if (coordinator->state == WindowCoordinatorStateFaulted) {
            status = WindowCoordinatorFaulted;
        } else if (coordinator->state == WindowCoordinatorStateLost) {
            status = WindowCoordinatorLost;
        }
        break;
    }
    if (stats_out != NULL) *stats_out = coordinator->stats;
    leave(coordinator);
    return status;
}

/**
 * @brief Moves a running coordinator to Lost: stops capture, drains the slot runtime's producers and
 *        invalidates the graph. Called with the busy gate held, from the owner thread only.
 */
static void enter_lost(window_coordinator_t *coordinator) {
    uint32_t drained = 0;
    coordinator->state = WindowCoordinatorStateLost;
    coordinator->outbound_count = 0u;
    coordinator->outbound_head = 0u;
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        coord_entry_t *entry = &coordinator->entries[index];
        if (entry->in_use != 0u && entry->kind == CoordEntryAnnounced) {
            capture_stop(coordinator, entry);
        }
    }
    if (waddle_slot_runtime_begin_drain(coordinator->slots, coordinator->epoch, 1u) == WaddleSlotOk) {
        coordinator->slots_draining = 1u;
        drained = 1u;
    }
    if (drained == 0u) coordinator->state = WindowCoordinatorStateFaulted;
    if (graph_ok(interop_window_graph_invalidate_all(coordinator->graph, coordinator->epoch, NULL)) == 0u) {
        coordinator->invalidate_pending = 1u;
    }
}

window_coordinator_status_t window_coordinator_mark_lost(window_coordinator_t *coordinator) {
    window_coordinator_status_t status = enter(coordinator);
    if (status != WindowCoordinatorOk) return status;
    if (coordinator->state != WindowCoordinatorStateRunning) {
        leave(coordinator);
        return WindowCoordinatorBadState;
    }
    enter_lost(coordinator);
    leave(coordinator);
    return WindowCoordinatorOk;
}

/**
 * @brief Ends every entry of the current epoch: stops capture, releases the native window and completes
 *        the graph quarantine. Caller guarantees the host proof (every pin of the epoch is drained).
 *
 * @param[in,out] coordinator Owner-thread coordinator with the host proof already given.
 */
static void retire_all_entries(window_coordinator_t *coordinator) {
    uint32_t index;
    for (index = 0; index < InteropMaxWindows; ++index) {
        coord_entry_t *entry = &coordinator->entries[index];
        interop_window_retire_proof_t proof;
        if (entry->in_use == 0u) continue;
        if (entry->reducer != NULL) waddle_window_state_destroy(&entry->reducer);
        if (entry->kind == CoordEntryAnnounced) {
            capture_stop(coordinator, entry);
            /* The slot runtime refuses to drain or be destroyed while an admitted window still holds its slots. */
            slots_retire(coordinator, entry->window, entry->generation);
        }
        release_native(coordinator, entry->window, entry->generation);
        proof.capture_stopped = 1u;
        proof.pins_drained = 1u;
        (void)interop_window_graph_complete_retire(coordinator->graph, coordinator->epoch, entry->window, entry->generation, &proof);
        memset(entry, 0, sizeof(*entry));
    }
}

/**
 * @brief Completes one graph RetireIntent without a wire message.
 *
 * Used by reconnect for generations that never had a coordinator entry (for example a window observed
 * while Lost whose Announce was never admitted). Capture was stopped when the transport was lost, and the
 * host proof of the new epoch drains every pin, so the proof is exact.
 *
 * @param[in,out] coordinator Owner-thread coordinator in the Lost state.
 * @param[in] event Staged or queued graph event; non-RetireIntent events are ignored.
 */
static void complete_retirement_event(window_coordinator_t *coordinator, const interop_window_event_t *event) {
    interop_window_retire_proof_t proof;
    if (event->kind != InteropWindowEventRetireIntent) return;
    proof.capture_stopped = 1u;
    proof.pins_drained = 1u;
    (void)interop_window_graph_complete_retire(coordinator->graph, coordinator->epoch, event->window,
                                               event->generation, &proof);
}

/**
 * @brief Applies the observer's retained protected Destroy entries to the graph before the observer is destroyed.
 *
 * The observer refuses to destroy while such entries remain. A full graph queue is drained by completing
 * the queued retirements, then the pass is repeated; the attempt count is bounded.
 *
 * @param[in,out] coordinator Owner-thread coordinator with its observer stopped.
 */
static void drain_observer_destroys(window_coordinator_t *coordinator) {
    for (uint32_t attempt = 0; attempt < 4u; ++attempt) {
        if (window_observer_drain_protected_destroys(coordinator->observer) != WindowObserverStatusGraphQueueFull) return;
        (void)complete_graph_retirements(coordinator);
    }
}

/**
 * @brief Completes every graph quarantine of the old epoch, including generations without an entry.
 *
 * Staged events are read first because reconnect discards them. Then every remaining Active generation
 * is moved to Retiring and the graph's queued RetireIntents are drained. Without this, the observer
 * refuses to destroy while the old graph still lists a window.
 *
 * @param[in,out] coordinator Owner-thread coordinator in the Lost state.
 * @return 0 when every quarantine was completed; 1 when the graph could not queue the retirements.
 */
static int complete_graph_retirements(window_coordinator_t *coordinator) {
    interop_window_event_t event;
    uint32_t index;
    for (index = 0; index < coordinator->staging_count; ++index) {
        complete_retirement_event(coordinator,
            &coordinator->staging[(coordinator->staging_head + index) % WaddleCoordinatorStagingCapacity]);
    }
    if (graph_ok(interop_window_graph_invalidate_all(coordinator->graph, coordinator->epoch, NULL)) == 0u) return 1;
    while (interop_window_graph_next_event(coordinator->graph, coordinator->epoch, &event) == InteropWindowGraphStatusOk) {
        complete_retirement_event(coordinator, &event);
    }
    return 0;
}

window_coordinator_status_t window_coordinator_reconnect(window_coordinator_t *coordinator, uint64_t new_epoch,
                                                         uint32_t host_buffers_destroyed) {
    window_coordinator_status_t status = enter(coordinator);
    if (status != WindowCoordinatorOk) return status;
    if (coordinator->state != WindowCoordinatorStateLost) {
        leave(coordinator);
        return WindowCoordinatorBadState;
    }
    if (new_epoch == 0u || new_epoch == coordinator->epoch) {
        leave(coordinator);
        return WindowCoordinatorInvalidArgument;
    }
    if (host_buffers_destroyed != 1u) {
        leave(coordinator);
        return WindowCoordinatorMissingProof;
    }
    /* The host proof releases every pin of the old epoch, so every quarantine can complete. */
    if (waddle_slot_runtime_confirm_host_drained(coordinator->slots, coordinator->epoch, 1u) != WaddleSlotOk) {
        coordinator->state = WindowCoordinatorStateFaulted;
        leave(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    retire_all_entries(coordinator);
    (void)window_observer_sync_tracked(coordinator->observer);
    if (coordinator->observer_started != 0u) {
        (void)window_observer_stop(coordinator->observer);
        coordinator->observer_started = 0u;
    }
    if (complete_graph_retirements(coordinator) != 0) {
        coordinator->state = WindowCoordinatorStateFaulted;
        leave(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    coordinator->staging_count = 0u;
    coordinator->staging_head = 0u;
    drain_observer_destroys(coordinator);
    if (window_observer_destroy(&coordinator->observer) != WindowObserverStatusOk) {
        coordinator->state = WindowCoordinatorStateFaulted;
        leave(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    (void)interop_window_graph_destroy(&coordinator->graph);
    (void)waddle_slot_runtime_destroy(&coordinator->slots);
    coordinator->outbound_count = 0u;
    coordinator->outbound_head = 0u;
    coordinator->head_retries = 0u;
    if (create_backends(coordinator, new_epoch) != 0) {
        coordinator->state = WindowCoordinatorStateFaulted;
        leave(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    coordinator->state = WindowCoordinatorStateRunning;
    if (start_observer(coordinator) != 0) {
        coordinator->state = WindowCoordinatorStateFaulted;
        leave(coordinator);
        return WindowCoordinatorBackendFailed;
    }
    leave(coordinator);
    return WindowCoordinatorOk;
}

uint32_t window_coordinator_state(const window_coordinator_t *coordinator) {
    if (coordinator == NULL) return 0u;
    if (coordinator->config.observer_api->current_thread_id(coordinator->config.observer_api_context) !=
        coordinator->owner_thread) {
        return 0u;
    }
    return coordinator->state;
}

uint32_t window_coordinator_entry_count(const window_coordinator_t *coordinator) {
    if (coordinator == NULL) return 0u;
    if (coordinator->config.observer_api->current_thread_id(coordinator->config.observer_api_context) !=
        coordinator->owner_thread) {
        return 0u;
    }
    return entry_count(coordinator);
}

window_coordinator_status_t window_coordinator_get_stats(const window_coordinator_t *coordinator,
                                                         window_coordinator_stats_t *stats) {
    if (coordinator == NULL || stats == NULL) return WindowCoordinatorInvalidArgument;
    if (coordinator->config.observer_api->current_thread_id(coordinator->config.observer_api_context) !=
        coordinator->owner_thread) {
        return WindowCoordinatorWrongThread;
    }
    *stats = coordinator->stats;
    return WindowCoordinatorOk;
}
