/**
 * @file window_state.h
 * @brief C ABI of the portable window-state reducer (src/interop/window_state.zig).
 *
 * Each function mirrors one public operation of the reducer. The machine is held behind an
 * opaque handle that is allocated by waddle_window_state_create and freed by
 * waddle_window_state_destroy. Plans are fixed-size, union-free values that the caller
 * returns verbatim to waddle_window_state_complete. The authoritative layouts, error
 * ordering and threading rules are in impl/seamless-interop/claude_45_percent/window_state_abi_contract.md.
 *
 * Common rules for every function:
 *  - The return value is a waddle_window_state_status_t. Ok is 0; every other value is an error.
 *  - Boolean fields are uint32_t and must be exactly 0 or 1. Any other value is InvalidArgument.
 *  - Enum-typed fields are carried as uint32_t so that out-of-range values can be detected.
 *    An unknown input kind is InvalidArgument; an unknown plan or action kind is InvalidPlan.
 *  - Caller pointers may be misaligned for their type; the ABI reads them by value.
 *  - Output storage is written only on the statuses documented for each function.
 */
#ifndef WaddleWindowStateH
#define WaddleWindowStateH

#include <stdint.h>

/** @brief Maximum number of actions in one plan. Mirrors the reducer constant MaxActions. */
#define WaddleWindowStateMaxActions 8

/**
 * @brief Result code of every window-state ABI call. Numeric values are frozen.
 *
 * Errors are reported in the same order the reducer checks them. Argument checks run first.
 */
typedef enum waddle_window_state_status_t {
    /** @brief The operation completed. Output storage, when documented, is written. */
    WaddleWindowStateOk = 0,
    /** @brief A required pointer is NULL, a boolean is not 0 or 1, or an input kind is unknown. */
    WaddleWindowStateInvalidArgument = 1,
    /** @brief create could not allocate the handle. The output handle is NULL. */
    WaddleWindowStateOutOfMemory = 2,
    /** @brief create received window_id 0 or generation 0. */
    WaddleWindowStateInvalidIdentity = 3,
    /**
     * @brief A guest state contains Activated or an undefined bit, or a host configure
     * contains a capability bit or an undefined bit.
     */
    WaddleWindowStateInvalidState = 4,
    /** @brief A configure has a zero extent paired with a nonzero extent, or an extent above 16384. */
    WaddleWindowStateInvalidGeometry = 5,
    /** @brief plan received a window_id or generation that differs from the bound identity. */
    WaddleWindowStateStaleWindow = 6,
    /** @brief complete received a plan whose base revision is no longer the machine revision. */
    WaddleWindowStateStalePlan = 7,
    /** @brief The window was retired by a Hide or Destroy observation. No later plan or completion succeeds. */
    WaddleWindowStateRetired = 8,
    /** @brief A host-side request was planned while the machine awaits a fresh guest observation. */
    WaddleWindowStateUnsynchronized = 9,
    /** @brief A maximize request was planned while the last guest observation lacks the Maximizable capability. */
    WaddleWindowStateUnsupportedStateRequest = 10,
    /** @brief The revision reached UINT64_MAX. The machine issues no further plans or completions. */
    WaddleWindowStateRevisionExhausted = 11,
    /**
     * @brief complete received a completed-action count larger than the plan, a success whose count
     * differs from the plan length, or a Retire completion that failed or reported actions.
     */
    WaddleWindowStateInvalidCompletion = 12,
    /**
     * @brief complete received a plan that is not a plan this handle issued: an unknown kind, an
     * action count above WaddleWindowStateMaxActions, a boolean field outside 0 or 1, or a seal mismatch.
     */
    WaddleWindowStateInvalidPlan = 13
} waddle_window_state_status_t;

/** @brief Input kind carried in waddle_window_state_input_t.kind. Numeric values are frozen. */
typedef enum waddle_window_state_input_kind_t {
    /** @brief Fresh guest observation; uses the state field. */
    WaddleWindowStateInputGuestObservation = 1,
    /** @brief Host compositor configure; uses the state and geometry fields. */
    WaddleWindowStateInputHostConfigure = 2,
    /** @brief Host maximize-policy request; uses the maximize field. */
    WaddleWindowStateInputHostStateRequest = 3,
    /** @brief Host close request; uses no field. */
    WaddleWindowStateInputCloseRequest = 4,
    /** @brief Guest hide observation that retires the window; uses no field. */
    WaddleWindowStateInputHideObserved = 5,
    /** @brief Guest destroy observation that retires the window; uses no field. */
    WaddleWindowStateInputDestroyObserved = 6
} waddle_window_state_input_kind_t;

/**
 * @brief One planning input. Fixed size 28 bytes, alignment 4, no implicit padding.
 *
 * Fields not used by the selected kind are ignored. The caller owns the value.
 */
typedef struct waddle_window_state_input_t {
    /** @brief A waddle_window_state_input_kind_t value. Any other value is InvalidArgument. */
    uint32_t kind;
    /** @brief Guest state bits for GuestObservation; host configure state bits for HostConfigure. */
    uint32_t state;
    /** @brief Signed caller-prepared x coordinate for HostConfigure, passed through unchanged. */
    int32_t x;
    /** @brief Signed caller-prepared y coordinate for HostConfigure, passed through unchanged. */
    int32_t y;
    /** @brief Width for HostConfigure. Zero only when height is also zero; otherwise 1 through 16384. */
    uint32_t width;
    /** @brief Height for HostConfigure. Zero only when width is also zero; otherwise 1 through 16384. */
    uint32_t height;
    /** @brief HostStateRequest only: 1 requests maximize, 0 requests unmaximize. Other values are InvalidArgument. */
    uint32_t maximize;
} waddle_window_state_input_t;

/** @brief Action kind carried in waddle_window_state_action_t.kind. Numeric values are frozen. */
typedef enum waddle_window_state_action_kind_t {
    /** @brief Ask the host compositor to minimize the toplevel (one-shot request). */
    WaddleWindowStateActionHostSetMinimized = 1,
    /** @brief Ask the host compositor to maximize the toplevel. */
    WaddleWindowStateActionHostSetMaximized = 2,
    /** @brief Ask the host compositor to unmaximize the toplevel. */
    WaddleWindowStateActionHostUnsetMaximized = 3,
    /** @brief Ask the host compositor to enter fullscreen. */
    WaddleWindowStateActionHostSetFullscreen = 4,
    /** @brief Ask the host compositor to leave fullscreen. */
    WaddleWindowStateActionHostUnsetFullscreen = 5,
    /** @brief Restore the guest window from minimized or maximized state. */
    WaddleWindowStateActionGuestRestore = 6,
    /** @brief Maximize the guest window. */
    WaddleWindowStateActionGuestMaximize = 7,
    /** @brief Minimize the guest window after preceding geometry actions. */
    WaddleWindowStateActionGuestMinimize = 8,
    /** @brief Enter guest fullscreen before geometry. */
    WaddleWindowStateActionGuestEnterFullscreen = 9,
    /** @brief Leave guest fullscreen before geometry. */
    WaddleWindowStateActionGuestLeaveFullscreen = 10,
    /** @brief Activate the guest window. */
    WaddleWindowStateActionGuestActivate = 11,
    /** @brief Apply the rectangle in x, y, width, height with the maximized flag. */
    WaddleWindowStateActionGuestSetGeometry = 12,
    /** @brief Post a guest WM_CLOSE request. This is not proof of retirement. */
    WaddleWindowStateActionGuestPostClose = 13
} waddle_window_state_action_kind_t;

/**
 * @brief One ordered adapter action. Fixed size 24 bytes, alignment 4, no implicit padding.
 *
 * Only SetGeometry uses x, y, width, height and maximized. For every other kind those fields are zero.
 */
typedef struct waddle_window_state_action_t {
    /** @brief A waddle_window_state_action_kind_t value. */
    uint32_t kind;
    /** @brief SetGeometry: signed x coordinate, unchanged from the configure. */
    int32_t x;
    /** @brief SetGeometry: signed y coordinate, unchanged from the configure. */
    int32_t y;
    /** @brief SetGeometry: width, 1 through 16384. */
    uint32_t width;
    /** @brief SetGeometry: height, 1 through 16384. */
    uint32_t height;
    /** @brief SetGeometry: 1 when the configure is maximized, else 0. */
    uint32_t maximized;
} waddle_window_state_action_t;

/** @brief Internal completion payload kind. Callers copy it verbatim and never interpret it. */
typedef enum waddle_window_state_pending_kind_t {
    /** @brief Payload: pending_state, pending_capabilities, pending_host_state. */
    WaddleWindowStatePendingGuestObservation = 1,
    /** @brief Payload: pending_state (resulting guest state). */
    WaddleWindowStatePendingHostConfigure = 2,
    /** @brief Payload: pending_state (0 or 1, the maximize policy). */
    WaddleWindowStatePendingHostStateRequest = 3,
    /** @brief No payload. */
    WaddleWindowStatePendingCloseRequest = 4,
    /** @brief No payload. */
    WaddleWindowStatePendingRetire = 5
} waddle_window_state_pending_kind_t;

/**
 * @brief A generation-bound action plan. Fixed size 248 bytes, alignment 8.
 *
 * The caller owns the value and must return it unmodified to waddle_window_state_complete.
 * Editing any field makes complete return InvalidPlan. Only the first action_count entries of
 * actions are meaningful; later entries are zero.
 */
typedef struct waddle_window_state_plan_t {
    /** @brief Protocol window identity the plan applies to. */
    uint64_t window_id;
    /** @brief Machine revision read when the plan was made. */
    uint64_t base_revision;
    /** @brief Count of initialized actions, 0 through WaddleWindowStateMaxActions. */
    uint64_t action_count;
    /** @brief Integrity tag computed by plan and checked by complete. Not an authenticator. */
    uint64_t seal;
    /** @brief Window generation the plan applies to. */
    uint32_t generation;
    /** @brief A waddle_window_state_pending_kind_t value. */
    uint32_t pending_kind;
    /** @brief Payload word; meaning depends on pending_kind. */
    uint32_t pending_state;
    /** @brief GuestObservation payload: guest capability bits. */
    uint32_t pending_capabilities;
    /** @brief GuestObservation payload: persistent host state after the actions succeed. */
    uint32_t pending_host_state;
    /** @brief Ordered actions. */
    waddle_window_state_action_t actions[WaddleWindowStateMaxActions];
} waddle_window_state_plan_t;

/** @brief Maximize-policy request as last accepted by the machine. */
typedef enum waddle_window_state_host_request_t {
    /** @brief No maximize-policy request has completed. */
    WaddleWindowStateHostRequestNone = 0,
    /** @brief The last completed request asked to unmaximize. */
    WaddleWindowStateHostRequestUnmaximize = 1,
    /** @brief The last completed request asked to maximize. */
    WaddleWindowStateHostRequestMaximize = 2
} waddle_window_state_host_request_t;

/**
 * @brief Read-only copy of the machine fields. Fixed size 56 bytes, alignment 8.
 *
 * Every state bit field uses the frozen protocol bits from window_state.zig. Boolean fields are 0 or 1.
 */
typedef struct waddle_window_state_snapshot_t {
    /** @brief Bound protocol window identity. */
    uint64_t window_id;
    /** @brief Current machine revision. */
    uint64_t revision;
    /** @brief Bound window generation. */
    uint32_t generation;
    /** @brief Last guest-reported minimized, maximized and fullscreen bits. */
    uint32_t observed_guest_state;
    /** @brief Last completed guest-native state. */
    uint32_t applied_guest_state;
    /** @brief Last completed persistent host state (maximized and fullscreen bits only). */
    uint32_t applied_host_state;
    /** @brief Last guest-reported capability bits. */
    uint32_t guest_capabilities;
    /** @brief A waddle_window_state_host_request_t value. */
    uint32_t host_request;
    /** @brief 1 when the machine accepts host-side requests, else 0. */
    uint32_t synced;
    /** @brief 1 after a successful close-request completion (diagnostic only), else 0. */
    uint32_t close_posted;
    /** @brief 1 after a matching Hide or Destroy completion, else 0. */
    uint32_t retired;
} waddle_window_state_snapshot_t;

/**
 * @brief Opaque owner of one machine. Only the functions in this header may use it.
 *
 * Obtained from waddle_window_state_create; released by waddle_window_state_destroy.
 */
typedef struct waddle_window_state_handle_t waddle_window_state_handle_t;

/**
 * @brief Create a machine bound to one window generation, initialized from its first guest observation.
 *
 * The machine is unsynchronized until the first GuestObservation plan is completed successfully.
 *
 * @param window_id      in: protocol window identity. Must be nonzero.
 * @param generation     in: window incarnation. Must be nonzero.
 * @param guest_state    in: guest state and capability bits. Must not contain Activated or undefined bits.
 * @param out_handle     out: non-NULL slot. Set to NULL first, then to the new handle on Ok. Never NULL on Ok.
 * @return Ok, InvalidArgument (out_handle NULL), InvalidIdentity, InvalidState, or OutOfMemory.
 * @note Ownership: the caller owns the handle until waddle_window_state_destroy.
 * @note Thread safety: independent calls on distinct handles are safe from any threads.
 */
waddle_window_state_status_t waddle_window_state_create(
    uint64_t window_id,
    uint32_t generation,
    uint32_t guest_state,
    waddle_window_state_handle_t **out_handle);

/**
 * @brief Free a handle and clear the caller's slot.
 *
 * @param handle_slot    in/out: non-NULL slot holding a handle or NULL. On Ok the slot is set to NULL.
 *                       When *handle_slot is already NULL the call is a successful no-op.
 * @return Ok, or InvalidArgument when handle_slot is NULL.
 * @note Ownership: after Ok, every copy of the former handle pointer is dangling and must not be used.
 * @note Thread safety: must not run concurrently with any other call on the same handle.
 */
waddle_window_state_status_t waddle_window_state_destroy(waddle_window_state_handle_t **handle_slot);

/**
 * @brief Plan one input for the bound window generation without changing the machine.
 *
 * Checks run in this order: arguments, window identity, retirement, revision exhaustion,
 * synchronization, then the input's own rules. On every status other than Ok, *out_plan is
 * all zero and cannot be completed.
 *
 * @param handle         in: non-NULL live handle.
 * @param window_id      in: identity the caller believes is current. Must equal the bound window_id.
 * @param generation     in: identity the caller believes is current. Must equal the bound generation.
 * @param input          in: non-NULL input value. Read only during the call.
 * @param out_plan       out: non-NULL plan slot. Written on every status.
 * @return Ok, InvalidArgument, StaleWindow, Retired, RevisionExhausted, Unsynchronized, InvalidState,
 *         InvalidGeometry, or UnsupportedStateRequest.
 * @note Thread safety: may run concurrently with other plan calls on the same handle only when no
 *       complete or destroy call runs on that handle at the same time.
 */
waddle_window_state_status_t waddle_window_state_plan(
    const waddle_window_state_handle_t *handle,
    uint64_t window_id,
    uint32_t generation,
    const waddle_window_state_input_t *input,
    waddle_window_state_plan_t *out_plan);

/**
 * @brief Commit the result of executing a plan's ordered prefix.
 *
 * @param handle            in/out: non-NULL live handle. Changed only on Ok.
 * @param plan              in: non-NULL plan exactly as returned by waddle_window_state_plan.
 * @param succeeded         in: 1 when every action in the plan succeeded, else 0.
 * @param completed_actions in: number of leading actions known to have completed. Must equal
 *                          action_count when succeeded is 1, and must not exceed action_count.
 * @return Ok, InvalidArgument, InvalidPlan, StaleWindow, Retired, StalePlan, InvalidCompletion, or
 *         RevisionExhausted. Every error leaves the machine unchanged.
 * @note A failed or partial completion advances the revision and makes the machine unsynchronized.
 * @note Thread safety: calls on one handle must be serialized by the caller.
 */
waddle_window_state_status_t waddle_window_state_complete(
    waddle_window_state_handle_t *handle,
    const waddle_window_state_plan_t *plan,
    uint32_t succeeded,
    uint64_t completed_actions);

/**
 * @brief Copy the machine fields into caller storage.
 *
 * @param handle        in: non-NULL live handle.
 * @param out_snapshot  out: non-NULL. Written only on Ok.
 * @return Ok or InvalidArgument.
 * @note Thread safety: must not run concurrently with complete or destroy on the same handle.
 */
waddle_window_state_status_t waddle_window_state_snapshot(
    const waddle_window_state_handle_t *handle,
    waddle_window_state_snapshot_t *out_snapshot);

#endif
