#ifndef WaddleAvWindowsH
#define WaddleAvWindowsH
#include "waddle/av_protocol.h"
#include <windows.h>
/** @brief Borrowed synchronous window notification callback.
 * @param[in] message Nonnull transient message, valid only during callback.
 * @param[in,out] context Optional caller-owned context retained through stop.
 * @return 0 accepted, 1 defer a CreateV2/CreateV3 until refresh, -1 delivery failure.
 * @note Runs on the start caller's message-pump thread; must not retain message.
 */
typedef int (*av_window_notify_t)(const av_message_t *message, void *context);
/** @brief Start tracking visible application windows for one target process tree root.
 * @param[in] process_id Nonzero process ID to select; only this process is tracked.
 * @param[in] notify Nonnull borrowed synchronous callback retained until stop.
 * @param[in,out] context Optional caller-owned callback context.
 * @return 0 on success, -1 on invalid arguments/hook registration failure.
 * @note Sets PER_MONITOR_AWARE_V2 on the caller thread until stop restores its
 * prior DPI context; refusal fails start. One active tracker per process; caller must pump Windows messages and
 * call av_windows_stop on the same thread. No heap allocation or ownership transfer.
 */
int av_windows_start(DWORD process_id, av_window_notify_t notify, void *context);
/** @brief Start bounded V3 input leases, initially unarmed, for one process.
 * @param[in] process_id Nonzero target PID; a lifetime HANDLE is retained internally.
 * @param[in] notify Nonnull borrowed synchronous callback, including global lease controls.
 * @param[in,out] context Optional caller-owned callback context, retained until stop.
 * @return 0 on success, -1 native/hook/clock/publication failure.
 * @note Owner message-pump thread only; fixed storage, no heap. Capture admission is
 * unchanged, while input requires exported ordinary ownerless windows. The caller
 * must invoke tick around loop work and bound its wait with timeout. Requires an
 * isolated interactive desktop; observed recovery is not an HWND security boundary.
 */
int av_windows_start_v3(DWORD process_id, av_window_notify_t notify, void *context);
/** @brief Resolve queued native observations and check the handshake deadline.
 * @return 0 healthy, -1 terminal; mandatory held-input cleanup occurs on failure.
 * @note Start caller's thread only, no parameters/allocation. Call at owner-loop
 * boundaries, never from notification callbacks. V2 retains legacy semantics.
 */
int av_windows_tick(void);
/** @brief Bound the next owner-loop wait by the fixed V3 handshake deadline.
 * @param[in] maximum_ms Nonnegative caller wait cap in milliseconds.
 * @return 0..maximum_ms wait cap, -1 terminal/expired/invalid clock or cap.
 * @note Owner thread only, allocation-free; never extends a deadline.
 */
int av_windows_timeout(int maximum_ms);
/** @brief Retry bounded window admission after transport/compositor leases clear.
 * @return 0 scanned, -1 inactive tracker, enumeration or delivery failure.
 * @note Start caller's thread only; no parameters/allocation; callback lifetimes
 * remain those of start. Existing unchanged windows do not emit duplicate events.
 */
int av_windows_refresh(void);
/** @brief Query whether the tracker may still perform session work.
 * @return 0 active and healthy, -1 inactive or latched delivery failure.
 * @note Tracker thread only; no parameters, allocation or native API calls.
 */
int av_windows_status(void);
/** @brief Remove hooks and tracked handles after stopping the message pump.
 * @return 0 all inputs released, hooks removed and DPI restored; -1 cleanup failure.
 * @note Same thread as start; idempotent. Failed unhook permanently prohibits restart.
 * Failed held releases remain recorded for the bounded teardown retry. No allocation.
 */
int av_windows_stop(void);
/** @brief Apply a host geometry/close request to a currently tracked window.
 * @param[in] message Nonnull borrowed validated control request.
 * @return 0 accepted or stale no-op, -1 malformed/type/Win32/session failure.
 * @note Tracker thread only; matches immutable identity before native calls and verifies process ownership.
 * Guest x/y remain unchanged; only size/fullscreen/minimize/WM_CLOSE supported.
 */
int av_windows_apply(const av_message_t *message);
/** @brief Apply canonical host input or V3 Ack through the selected start mode.
 * @param[in] message Nonnull borrowed decoded input/Ack; retained only during call.
 * @return 0 accepted or stale identity safely ignored, -1 replayed/unfocused/OS or
 * UIPI failure, fatal to peer.
 * @note Tracker thread only; no allocation. V3 stale epochs never trigger native
 * reconciliation; callers separately invoke tick at loop boundaries. stop releases
 * successfully held input. Requires an isolated interactive guest desktop; SendInput/foreground validation
 * are not atomic and do not establish an OS security boundary.
 */
int av_windows_input(const av_message_t *message);
#endif
