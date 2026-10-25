/**
 * @file window_coordinator.h
 * @brief Guest window runtime: observer, graph, slots, capture seam, wire and window-state reducer (TODO #10 C1-C3, #13 guest half).
 *
 * The coordinator owns one session epoch's graph, observer, slot runtime and window-state adapter. Each
 * tick it pumps the observer, drains graph events in order into a bounded staging queue, admits or
 * retires windows (slots, capture seam, adapter, reducer), publishes frames through the slot runtime
 * publish path, applies host frames (WindowConfigure, WindowClose, FrameRelease, popups) and writes every
 * outgoing frame to a bounded FIFO that is flushed through the wire seam. The wire, the capture and the
 * window-state native table are seams: production supplies the real ones, the tests supply doubles.
 * Contract: impl/seamless-interop/claude_45_percent/window_coordinator_contract.md.
 *
 * Ownership. window_coordinator_create performs exactly one allocation, which holds the coordinator, its
 * staging queue and its outbound FIFO. The graph, observer, slot runtime and reducer handles are created
 * here and freed by window_coordinator_destroy. The config, the io table, the capture table and every
 * context pointer are borrowed and must outlive the coordinator.
 *
 * Threading. The owner thread is the thread that calls window_coordinator_create. Every other function
 * must run on that thread. A call from another thread returns WrongThread and touches nothing. A call made
 * while another coordinator entry is active on the owner thread returns Reentrant. The io, capture and
 * native callbacks run on the owner thread inside a tick.
 */
#ifndef WaddleWindowCoordinatorH
#define WaddleWindowCoordinatorH

#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_protocol.h"
#include "waddle/interop_window_graph.h"
#include "window_observer_windows.h"
#include "window_state.h"
#include "window_state_adapter_windows.h"
#include "interop_slot_runtime.h"

/** @brief Capacity of the graph-event staging queue. Equal to the graph's ring capacity. */
#define WaddleCoordinatorStagingCapacity 256u
/** @brief Capacity of the outbound frame FIFO. */
#define WaddleCoordinatorOutboundCapacity 256u
/** @brief Largest encoded outbound frame. Every message the coordinator sends fits. */
#define WaddleCoordinatorMaxFrameBytes 2048u
/** @brief Failed admission attempts for one event before the coordinator faults. */
#define WaddleCoordinatorMaxAdmissionRetries 64u
/** @brief Capture start and frame publish rounds per window per tick. */
#define WaddleCoordinatorFramesPerTick 1u

/** @brief Result code of every coordinator function. Numeric values are frozen. */
typedef enum window_coordinator_status_t {
    /** @brief Completed. */
    WindowCoordinatorOk = 0,
    /** @brief A required pointer is NULL or a field is invalid. Nothing changed. */
    WindowCoordinatorInvalidArgument = 1,
    /** @brief A required allocation failed in create. */
    WindowCoordinatorOutOfMemory = 2,
    /** @brief Called from a thread other than the owner. Nothing touched. */
    WindowCoordinatorWrongThread = 3,
    /** @brief Called while another entry was active. Nothing touched. */
    WindowCoordinatorReentrant = 4,
    /** @brief The negotiated capabilities lack InteropCapWindows; the coordinator sends and admits nothing. */
    WindowCoordinatorNoCapability = 5,
    /** @brief The call is illegal in the current state (for example reconnect while not lost). Nothing changed. */
    WindowCoordinatorBadState = 6,
    /** @brief reconnect was given no host-buffer-destroyed proof. Nothing changed. */
    WindowCoordinatorMissingProof = 7,
    /** @brief A required graph, observer, slot or adapter operation failed; see the stats counters. */
    WindowCoordinatorBackendFailed = 8,
    /** @brief destroy refused: entries, pins or a running observer remain. Nothing freed. */
    WindowCoordinatorBusy = 9,
    /** @brief The coordinator reached a permanent fault and stops admitting and sending. */
    WindowCoordinatorFaulted = 10,
    /** @brief The transport is lost. Nothing was sent by this pass; the coordinator waits for reconnect. */
    WindowCoordinatorLost = 11
} window_coordinator_status_t;

/** @brief Result of one wire send. */
typedef enum window_coordinator_send_result_t {
    /** @brief The frame was accepted by the wire. */
    WindowCoordinatorSent = 0,
    /** @brief The wire cannot take the frame now. It stays at the head of the FIFO and is retried. */
    WindowCoordinatorBackpressure = 1,
    /** @brief The transport is gone. The coordinator enters the Lost state. */
    WindowCoordinatorDisconnected = 2
} window_coordinator_send_result_t;

/**
 * @brief Wire seam: the coordinator's only transport. Borrowed; the callbacks run on the owner thread.
 */
typedef struct window_coordinator_io_t {
    /** @brief Opaque pointer passed to both callbacks. */
    void *context;
    /**
     * @brief Send one complete frame (header and payload). The bytes are valid only during the call.
     * @param[in,out] context The io context.
     * @param[in] frame Non-NULL, length bytes.
     * @param[in] length Frame bytes, at most WaddleCoordinatorMaxFrameBytes.
     * @return Sent, Backpressure or Disconnected.
     */
    window_coordinator_send_result_t (*send_frame)(void *context, const uint8_t *frame, size_t length);
    /**
     * @brief Receive at most one complete host frame into a caller buffer.
     * @param[in,out] context The io context.
     * @param[out] frame Non-NULL buffer of capacity bytes.
     * @param[in] capacity Bytes writable in frame.
     * @param[out] length Non-NULL; the frame length when the return value is 1.
     * @return 1 when a frame was copied, 0 when none is pending, -1 when the transport is gone.
     */
    int (*receive_frame)(void *context, uint8_t *frame, size_t capacity, size_t *length);
} window_coordinator_io_t;

/**
 * @brief Capture seam. Borrowed; the callbacks run on the owner thread.
 *
 * The coordinator calls start after the slots and the adapter are admitted, stop before any retirement
 * is sent, and write_frame between prepare_publish and commit_publish of one frame.
 */
typedef struct window_capture_ops_t {
    /** @brief Opaque pointer passed to every callback. */
    void *context;
    /**
     * @brief Start capturing one admitted generation.
     * @return 0 on success. Nonzero means the generation is not announced; its slots are released.
     */
    int (*start)(void *context, uint64_t window, uint32_t generation, uint32_t width, uint32_t height);
    /** @brief Stop capturing one generation. Idempotent for an unknown generation. */
    void (*stop)(void *context, uint64_t window, uint32_t generation);
    /**
     * @brief Produce one frame into the slot described by view. The view is a snapshot; no pointer is
     *        exposed. Return codes: 0 frame written; 1 failed after the caller's Free-to-Writing
     *        transition (the coordinator cancels the entered writer); 2 failed before any transition
     *        (the coordinator cancels the unentered claim).
     */
    int (*write_frame)(void *context, uint64_t window, uint32_t generation, uint8_t slot, uint64_t sequence,
                       const waddle_slot_view_t *view);
    /**
     * @brief Optional. Told after the slot runtime accepted a new geometry for a capturing generation, so the
     *        capture source can change its size. NULL means the source is not resized.
     */
    void (*resize)(void *context, uint64_t window, uint32_t generation, uint32_t width, uint32_t height);
} window_capture_ops_t;

/** @brief Configuration. Copied by value; the pointers inside are borrowed and must outlive the coordinator. */
typedef struct window_coordinator_config_t {
    /** @brief Nonzero epoch of this session; the graph, observer and slot runtime are created for it. */
    uint64_t session_epoch;
    /** @brief interop_window_uac_mode_t, 0 to 2. Used for the graph and the observer alike. */
    uint32_t uac_mode;
    /** @brief Nonzero process id of the agent; its windows are AgentOwned. */
    uint32_t own_process_id;
    /** @brief Negotiated capability bits (InteropTagCapabilities of HelloAck). Windows must be set to run. */
    uint64_t negotiated_capabilities;
    /** @brief Required application-key lookup for the graph. Runs only on the owner thread. */
    interop_window_app_key_lookup_t app_key_lookup;
    /** @brief Context of app_key_lookup. */
    void *app_key_context;
    /** @brief Graph clock. NULL selects the graph default clock. Also used for timeouts. */
    interop_window_now_ms_t now_ms;
    /** @brief Context of now_ms. */
    void *now_ms_context;
    /** @brief Observer native table. Required (the Win32 table is the production choice on Windows). */
    const window_observer_api_t *observer_api;
    /** @brief Context of observer_api. */
    void *observer_api_context;
    /** @brief Window-state native table. Required. */
    const window_state_native_api_t *native_api;
    /** @brief Context of native_api. */
    void *native_api_context;
    /** @brief Wire seam. Required. */
    const window_coordinator_io_t *io;
    /** @brief Capture seam. Required. */
    const window_capture_ops_t *capture;
} window_coordinator_config_t;

/** @brief Lifecycle state. Values are stable. */
typedef enum window_coordinator_state_t {
    /** @brief Created without the Windows capability: nothing is observed or sent. */
    WindowCoordinatorStateIdle = 1,
    /** @brief Running: observation, admission, publish and host frames are processed. */
    WindowCoordinatorStateRunning = 2,
    /** @brief The transport was lost: capture is stopped, nothing is sent, and quarantines stay until reconnect. */
    WindowCoordinatorStateLost = 3,
    /** @brief A permanent fault: nothing is admitted or sent. */
    WindowCoordinatorStateFaulted = 4
} window_coordinator_state_t;

/** @brief Cumulative counters. Start at 0 at create and saturate at UINT64_MAX. */
typedef struct window_coordinator_stats_t {
    /** @brief WindowAnnounce frames queued. */
    uint64_t announces;
    /** @brief WindowUpdate frames queued. */
    uint64_t updates;
    /** @brief WindowRetire frames queued. */
    uint64_t retires;
    /** @brief FrameReady frames queued. */
    uint64_t frames_published;
    /** @brief Host FrameRelease frames that released a pin. */
    uint64_t releases_applied;
    /** @brief Host FrameRelease frames that named a generation the coordinator does not hold, or a pin that does not match. */
    uint64_t releases_ignored;
    /** @brief Error{StaleWindow} frames queued for host frames that name no exported generation. */
    uint64_t stale_replies;
    /** @brief Host WindowConfigure frames applied to the reducer. */
    uint64_t configures;
    /** @brief Host WindowClose frames applied to the reducer. */
    uint64_t closes;
    /** @brief Host popup frames accepted without effect (popups are not realized in this tranche). */
    uint64_t popup_frames;
    /** @brief Host frames that failed validation or had an unknown type. */
    uint64_t host_frames_rejected;
    /** @brief Admissions held because a retire barrier was pending or a resource was full. */
    uint64_t admissions_held;
    /** @brief Admission steps that failed for a resource (slots, adapter, reducer, capture). */
    uint64_t admission_failures;
    /** @brief Generations the graph announced that the coordinator could not serve, and retired without a wire message. */
    uint64_t phantoms;
    /** @brief Retire barriers completed by complete_retire. */
    uint64_t retire_completed;
    /** @brief Frame publishes skipped because no slot or no FIFO room was free. */
    uint64_t frames_skipped;
    /** @brief Frame writes that failed (the entered or unentered claim was cancelled). */
    uint64_t capture_failures;
    /** @brief Wire sends that returned Backpressure. */
    uint64_t backpressure;
    /** @brief Wire sends that returned Disconnected. */
    uint64_t disconnects;
    /** @brief Graph events staged in order. */
    uint64_t events_staged;
    /** @brief Reducer plans or completions that were refused or failed. */
    uint64_t reducer_failures;
} window_coordinator_stats_t;

/** @brief Opaque coordinator. Allocated by create, freed by destroy; layout private to the C file. */
typedef struct window_coordinator_t window_coordinator_t;

/**
 * @brief Create a coordinator for one session epoch. Creates the graph, the observer (bound and started
 *        when the Windows capability is negotiated) and the slot runtime.
 *
 * @param[in] config Non-NULL, copied. Required fields are checked; a NULL seam is InvalidArgument.
 * @param[out] out_coordinator Non-NULL slot, set to NULL first and to the new coordinator on Ok.
 * @return Ok, InvalidArgument, OutOfMemory, or BackendFailed (a graph, observer or slot create failed).
 * @note Owner thread: the calling thread. Allocates exactly one object.
 */
window_coordinator_status_t window_coordinator_create(const window_coordinator_config_t *config,
                                                      window_coordinator_t **out_coordinator);

/**
 * @brief Free a coordinator. Refused while any entry is admitted; the caller must have retired them.
 *
 * An Idle coordinator never shared a mapping with a host, so no proof is needed. In every other
 * state the caller must assert that the host has destroyed every buffer of this epoch. That assertion
 * is the only way the slot runtime's drain completes.
 *
 * @param[in,out] coordinator_slot Non-NULL slot. A NULL *coordinator_slot is a successful no-op.
 * @param[in] host_buffers_destroyed Exactly 1 when the host confirmed its buffers are destroyed; 0 otherwise.
 * @return Ok; Busy (entries remain or the observer could not stop; nothing freed); MissingProof (not Idle
 *         and the proof is 0; nothing freed); InvalidArgument; WrongThread; Reentrant.
 */
window_coordinator_status_t window_coordinator_destroy(window_coordinator_t **coordinator_slot,
                                                       uint32_t host_buffers_destroyed);

/**
 * @brief Run one service pass: observe, stage, admit or retire, publish, handle host frames, flush.
 *
 * @param[in,out] coordinator Non-NULL owner-thread coordinator.
 * @param[in] budget Observer keys per pump call, at least 1.
 * @param[out] stats_out Nullable; receives a copy of the counters after the pass.
 * @return Ok; NoCapability (Idle, nothing done); Faulted; Lost (the transport is gone; nothing was sent
 *         by this pass); WrongThread; Reentrant; InvalidArgument.
 */
window_coordinator_status_t window_coordinator_tick(window_coordinator_t *coordinator, uint32_t budget,
                                                    window_coordinator_stats_t *stats_out);

/**
 * @brief Signal that the transport was lost, without waiting for a send to report it.
 *
 * Stops every capture, drains the slot runtime's producers, and invalidates the graph (every
 * generation becomes a quarantined retirement without a wire message). Quarantines remain until
 * reconnect supplies host-drain proof: transport loss alone is never proof.
 *
 * @return Ok, or BadState when not Running, or WrongThread, Reentrant, InvalidArgument.
 */
window_coordinator_status_t window_coordinator_mark_lost(window_coordinator_t *coordinator);

/**
 * @brief Start a fresh session epoch after the previous session has safely ended.
 *
 * The caller asserts that every host buffer of the previous epoch is destroyed. That assertion releases
 * the previous epoch's pins, completes every quarantined retirement, and destroys the old graph,
 * observer and slot runtime. A new graph, observer and slot runtime are created for new_epoch. Admission
 * then re-announces every visible window through the normal refresh.
 *
 * @param[in,out] coordinator Non-NULL owner-thread coordinator, in the Lost state.
 * @param[in] new_epoch Nonzero epoch different from the current one.
 * @param[in] host_buffers_destroyed Exactly 1 when the host confirmed its buffers are destroyed; 0 is MissingProof.
 * @return Ok, BadState (not Lost), MissingProof, InvalidArgument, BackendFailed, WrongThread, Reentrant.
 */
window_coordinator_status_t window_coordinator_reconnect(window_coordinator_t *coordinator, uint64_t new_epoch,
                                                         uint32_t host_buffers_destroyed);

/**
 * @brief Read the lifecycle state.
 * @param[in] coordinator Non-NULL; owner thread only, otherwise returns 0.
 * @return A window_coordinator_state_t value, or 0 when the arguments are invalid.
 */
uint32_t window_coordinator_state(const window_coordinator_t *coordinator);

/**
 * @brief Count the admitted (announced or phantom) generations the coordinator holds.
 * @param[in] coordinator Non-NULL; owner thread only, otherwise 0.
 * @return The number of table entries in use.
 */
uint32_t window_coordinator_entry_count(const window_coordinator_t *coordinator);

/**
 * @brief Copy the counters.
 * @param[in] coordinator Non-NULL owner-thread coordinator.
 * @param[out] stats Non-NULL.
 * @return Ok, InvalidArgument, WrongThread.
 */
window_coordinator_status_t window_coordinator_get_stats(const window_coordinator_t *coordinator,
                                                         window_coordinator_stats_t *stats);

#endif
