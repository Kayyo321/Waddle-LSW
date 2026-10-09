#ifndef WaddleAvWindowsH
#define WaddleAvWindowsH
#include "waddle/av_protocol.h"
#include <windows.h>
/** @brief Borrowed synchronous window notification callback.
 * @param[in] message Nonnull transient message, valid only during callback.
 * @param[in,out] context Optional caller-owned context retained through stop.
 * @return 0 accepted, 1 defer a Create until refresh, -1 delivery failure.
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
/** @brief Retry bounded window admission after transport/compositor leases clear.
 * @return 0 scanned, -1 inactive tracker, enumeration or delivery failure.
 * @note Start caller's thread only; no parameters/allocation; callback lifetimes
 * remain those of start. Existing unchanged windows do not emit duplicate events.
 */
int av_windows_refresh(void);
/** @brief Remove hooks and tracked handles after stopping the message pump.
 * @return 0 all inputs released and DPI restored, -1 OS release or DPI restore failure.
 * @note Same thread as start; idempotent. No parameters or heap allocation.
 */
int av_windows_stop(void);
/** @brief Apply a host geometry/close request to a currently tracked window.
 * @param[in] message Nonnull borrowed validated control request.
 * @return 0 on success, -1 on stale handle/type/Win32 failure.
 * @note Tracker thread only; validates process ownership before using HWND.
 * Guest x/y remain unchanged; only size/fullscreen/minimize/WM_CLOSE supported.
 */
int av_windows_apply(const av_message_t *message);
/** @brief Apply canonical host input through tracked foreground SendInput.
 * @param[in] message Nonnull borrowed decoded input; retained only during call.
 * @return 0 accepted or stale identity safely ignored, -1 replayed/unfocused/OS or
 * UIPI failure, fatal to peer.
 * @note Tracker thread only; no allocation. stop releases successfully held input.
 * Requires an isolated interactive guest desktop; SendInput/foreground validation
 * are not atomic and do not establish an OS security boundary.
 */
int av_windows_input(const av_message_t *message);
#endif
