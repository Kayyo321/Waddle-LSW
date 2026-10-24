/** @file interop_slot_runtime.h @brief Opaque C boundary for slot lifetime bookkeeping. */
#ifndef WaddleInteropSlotRuntimeH
/** @brief Include guard; compile-time marker with no ownership. */
#define WaddleInteropSlotRuntimeH
#include <stddef.h>
#include <stdint.h>

/** @brief Fixed result code for one slot-runtime ABI call. */
typedef enum waddle_slot_status_t {
    /** Successful operation. */
    WaddleSlotOk = 0,
    /** Null, misaligned, wrapped, overlapping, or invalid scalar argument. */
    WaddleSlotInvalidArgument = 1,
    /** C allocator could not allocate the opaque runtime. */
    WaddleSlotOutOfMemory = 2,
    /** Creation supplied epoch zero. */
    WaddleSlotInvalidSessionEpoch = 3,
    /** Captured epoch does not match this runtime. */
    WaddleSlotWrongSessionEpoch = 4,
    /** Window identity is zero. */
    WaddleSlotInvalidWindow = 5,
    /** Window generation is zero. */
    WaddleSlotInvalidGeneration = 6,
    /** Window generation was already consumed or is stale. */
    WaddleSlotStaleGeneration = 7,
    /** Window identity is already admitted. */
    WaddleSlotDuplicateWindow = 8,
    /** No descriptor triple is available. */
    WaddleSlotNoWindowCapacity = 9,
    /** Window and generation pair is not admitted. */
    WaddleSlotUnknownWindow = 10,
    /** Slot is outside 0 through 2. */
    WaddleSlotInvalidSlot = 11,
    /** Geometry is invalid. */
    WaddleSlotInvalidGeometry = 12,
    /** Heap has no suitable contiguous run. */
    WaddleSlotHeapFull = 13,
    /** Heap reservation identity space is exhausted. */
    WaddleSlotTicketExhausted = 14,
    /** Another replacement triple is pending. */
    WaddleSlotBusy = 15,
    /** Slot capacity cannot satisfy the requested geometry. */
    WaddleSlotSlotTooSmall = 16,
    /** This window already has a provisional writer. */
    WaddleSlotWriterAlreadyActive = 17,
    /** No matching provisional writer exists. */
    WaddleSlotNoWriter = 18,
    /** Caller transition evidence disagrees with the provisional writer. */
    WaddleSlotWriterStateMismatch = 19,
    /** Sequence is zero, reused, or non-increasing. */
    WaddleSlotInvalidSequence = 20,
    /** Sequence reached its maximum and cannot wrap. */
    WaddleSlotSequenceExhausted = 21,
    /** Release identity or sequence does not match a published pin. */
    WaddleSlotWrongRelease = 22,
    /** Slot or window remains pinned or has a provisional writer. */
    WaddleSlotPinned = 23,
    /** Operation is invalid in the current lifecycle. */
    WaddleSlotInvalidLifecycle = 24,
    /** Required producer-stop or host-drain proof was not supplied. */
    WaddleSlotMissingProof = 25,
    /** Retained heap reservation state is inconsistent. */
    WaddleSlotHeapStateCorrupt = 26,
    /** Runtime is fail-closed and permits only drain. */
    WaddleSlotFailedClosed = 27
} waddle_slot_status_t;

/** @brief Opaque allocation-owned handle; only this API may inspect or destroy it.
 * @note Borrowed copies expire at destroy. Caller joins asynchronous work before
 * drain and never uses a stale pointer; argument checks cannot prove allocation
 * liveness. All calls on one handle are externally serialized. */
typedef struct waddle_slot_runtime_handle_t waddle_slot_runtime_handle_t;

/** @brief Scalar snapshot for one descriptor; contains no heap token or pointer. */
typedef struct waddle_slot_view_t {
    /** Frozen table descriptor index. */
    uint32_t descriptor_index;
    /** Window identity. */
    uint64_t window;
    /** Window incarnation generation. */
    uint64_t generation;
    /** Clamped frame width. */
    uint32_t width;
    /** Clamped frame height. */
    uint32_t height;
    /** 64-byte aligned row stride. */
    uint32_t stride;
    /** Exact unrounded pixel payload byte count. */
    uint64_t pixel_bytes;
    /** Mapping-relative start offset; no pointer is exposed. */
    uint64_t data_offset;
    /** Page-rounded frame buffer capacity in bytes. */
    uint32_t data_bytes;
} waddle_slot_view_t;

/** @brief Create one runtime for a fresh nonzero session epoch.
 * @param[in] session_epoch Nonzero unique epoch; caller guarantees old host drain before reuse.
 * @param[out] out_handle Nonnull aligned pointer to a null handle; unchanged on failure.
 * @return WaddleSlotOk, WaddleSlotInvalidSessionEpoch, WaddleSlotInvalidArgument or WaddleSlotOutOfMemory.
 * @ownership Success transfers one opaque allocation to *out_handle; destroy it after drain.
 * @thread_safety Returned handle requires one externally serialized event-loop thread. */
waddle_slot_status_t waddle_slot_runtime_create(uint64_t session_epoch,
                                                waddle_slot_runtime_handle_t **out_handle);

/** @brief Destroy only after successful host-drain confirmation; NULL handle is an idempotent no-op.
 * @param[in,out] handle Nonnull aligned pointer to caller-owned handle; successful destruction NULLs it.
 * @return WaddleSlotOk or WaddleSlotInvalidArgument/WaddleSlotInvalidLifecycle.
 * @ownership No memory is freed and handle is unchanged unless host drain completed.
 * @thread_safety Caller serializes with all operations on the handle. */
waddle_slot_status_t waddle_slot_runtime_destroy(waddle_slot_runtime_handle_t **handle);

/** @brief Admit a window and copy its three scalar descriptor views.
 * @param[in,out] handle Nonnull live opaque runtime.
 * @param[in] session_epoch Exact captured runtime epoch.
 * @param[in] window Nonzero window identity; @param[in] generation Strictly increasing nonzero generation.
 * @param[in] width Requested width; @param[in] height Requested height (both are clamped by layout policy).
 * @param[out] out_views Nonnull aligned array of three disjoint caller-owned views; unchanged on failure.
 * @return WaddleSlotOk or a fixed status describing validation/runtime failure.
 * @ownership Runtime retains reservations; views are snapshots only and contain no releasable token.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_admit_window(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint32_t width, uint32_t height,
    waddle_slot_view_t out_views[3]);

/** @brief Update desired geometry or transactionally stage a replacement triple.
 * @param[in,out] handle Nonnull live opaque runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Exact window; @param[in] generation Exact incarnation.
 * @param[in] width Requested width; @param[in] height Requested height.
 * @return WaddleSlotOk or a fixed status; mismatch leaves runtime unchanged.
 * @ownership Runtime owns all reservations; no pointers are returned.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_request_geometry(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint32_t width, uint32_t height);

/** @brief Reserve a provisional writer claim before Free-to-Writing.
 * @param[in,out] handle Nonnull live opaque runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Exact window; @param[in] generation Exact incarnation.
 * @param[in] slot Slot number 0..2; @param[in] frame_sequence Nonzero increasing sequence.
 * @param[out] out_view Nonnull aligned disjoint scalar view; unchanged on failure.
 * @return WaddleSlotOk or a fixed status; stale epoch leaves state and output unchanged.
 * @ownership The snapshot has no token; runtime retains reservation and claim.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_prepare_publish(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence,
    waddle_slot_view_t *out_view);

/** @brief Record caller-confirmed Free-to-Writing success.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Window; @param[in] generation Incarnation; @param[in] slot Slot 0..2.
 * @param[in] frame_sequence Exact provisional sequence.
 * @return WaddleSlotOk or a fixed status; no shared atomic is performed.
 * @ownership No ownership transfer; caller must report the real native transition result.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_mark_writer_entered(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence);

/** @brief Commit a pin after caller-confirmed Writing-to-Ready.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Window; @param[in] generation Incarnation; @param[in] slot Slot 0..2.
 * @param[in] frame_sequence Exact published sequence.
 * @return WaddleSlotOk or a fixed status; current-epoch publication corruption may fail-close.
 * @ownership Runtime retains the published pin until matching release or proven drain.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_commit_publish(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence);

/** @brief Cancel only a provisional claim whose Free-to-Writing CAS never succeeded.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Window; @param[in] generation Incarnation; @param[in] slot Slot 0..2.
 * @param[in] frame_sequence Exact provisional sequence.
 * @return WaddleSlotOk or a fixed status; stale epoch leaves state unchanged.
 * @ownership Sequence remains consumed; reservations remain runtime-owned.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_cancel_unentered(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence);

/** @brief Cancel an entered writer after caller-confirmed Writing-to-Free.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Window; @param[in] generation Incarnation; @param[in] slot Slot 0..2.
 * @param[in] frame_sequence Exact provisional sequence.
 * @return WaddleSlotOk or a fixed status; stale epoch leaves state unchanged.
 * @ownership Runtime retains reservations; this call performs no atomic operation.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_cancel_entered(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence);

/** @brief Release exactly one pin after matching FrameRelease validation.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact epoch.
 * @param[in] window Window; @param[in] generation Incarnation; @param[in] slot Slot 0..2.
 * @param[in] frame_sequence Exact published sequence.
 * @return WaddleSlotOk or a fixed status; observing shared Free alone is insufficient.
 * @ownership Matching release clears only the corresponding runtime pin.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_release_frame(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation, uint8_t slot, uint64_t frame_sequence);

/** @brief Retire a window and quarantine its descriptor triple until all claims/pins end.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] window Exact window; @param[in] generation Exact incarnation.
 * @return WaddleSlotOk or a fixed status; stale epoch leaves state unchanged.
 * @ownership Runtime releases reservations only after claims/pins are gone.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_retire_window(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch,
    uint64_t window, uint64_t generation);

/** @brief Begin drain after every producer has stopped and joined.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] producers_stopped Fixed uint32 proof value, exactly 0 or 1.
 * @return WaddleSlotOk or a fixed status; invalid proof values are rejected.
 * @ownership Does not free pins or reservations.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_begin_drain(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch, uint32_t producers_stopped);

/** @brief Confirm host buffers are destroyed after producer drain.
 * @param[in,out] handle Nonnull live runtime; @param[in] session_epoch Exact captured epoch.
 * @param[in] host_buffers_destroyed Fixed uint32 proof value, exactly 0 or 1.
 * @return WaddleSlotOk or a fixed status; success enables proof-gated destroy.
 * @ownership Runtime releases reservations only after the underlying proof succeeds.
 * @thread_safety One externally serialized event-loop thread per handle. */
waddle_slot_status_t waddle_slot_runtime_confirm_host_drained(
    waddle_slot_runtime_handle_t *handle, uint64_t session_epoch, uint32_t host_buffers_destroyed);

#if defined(__cplusplus)
#define WaddleSlotStaticAssert static_assert
#else
#define WaddleSlotStaticAssert _Static_assert
#endif
WaddleSlotStaticAssert(sizeof(waddle_slot_status_t) == 4, "slot status ABI width");
WaddleSlotStaticAssert(sizeof(void *) == sizeof(uint64_t), "supported opaque pointer ABI width");
WaddleSlotStaticAssert(WaddleSlotOk == 0 && WaddleSlotInvalidArgument == 1 &&
                       WaddleSlotOutOfMemory == 2 && WaddleSlotInvalidSessionEpoch == 3 &&
                       WaddleSlotWrongSessionEpoch == 4 && WaddleSlotInvalidWindow == 5 &&
                       WaddleSlotInvalidGeneration == 6 && WaddleSlotStaleGeneration == 7 &&
                       WaddleSlotDuplicateWindow == 8 && WaddleSlotNoWindowCapacity == 9 &&
                       WaddleSlotUnknownWindow == 10 && WaddleSlotInvalidSlot == 11 &&
                       WaddleSlotInvalidGeometry == 12 && WaddleSlotHeapFull == 13 &&
                       WaddleSlotTicketExhausted == 14 && WaddleSlotBusy == 15 &&
                       WaddleSlotSlotTooSmall == 16 && WaddleSlotWriterAlreadyActive == 17 &&
                       WaddleSlotNoWriter == 18 && WaddleSlotWriterStateMismatch == 19 &&
                       WaddleSlotInvalidSequence == 20 && WaddleSlotSequenceExhausted == 21 &&
                       WaddleSlotWrongRelease == 22 && WaddleSlotPinned == 23 &&
                       WaddleSlotInvalidLifecycle == 24 && WaddleSlotMissingProof == 25 &&
                       WaddleSlotHeapStateCorrupt == 26 && WaddleSlotFailedClosed == 27,
                       "slot status ABI values");
WaddleSlotStaticAssert(sizeof(waddle_slot_view_t) == 64, "slot view ABI size");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, descriptor_index) == 0, "slot view descriptor offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, window) == 8, "slot view window offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, generation) == 16, "slot view generation offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, width) == 24, "slot view width offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, height) == 28, "slot view height offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, stride) == 32, "slot view stride offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, pixel_bytes) == 40, "slot view pixel bytes offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, data_offset) == 48, "slot view data offset");
WaddleSlotStaticAssert(offsetof(waddle_slot_view_t, data_bytes) == 56, "slot view data bytes offset");
#undef WaddleSlotStaticAssert
#endif
