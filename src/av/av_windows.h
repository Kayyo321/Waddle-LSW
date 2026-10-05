#ifndef WaddleAvWindowsH
#define WaddleAvWindowsH
#include "waddle/av_protocol.h"
#include <windows.h>
/** @brief Borrowed synchronous window notification callback.
 * @param[in] message Nonnull transient message, valid only during callback.
 * @param[in,out] context Optional caller-owned context retained through stop.
 * @return 0 on success, -1 to record delivery failure.
 * @note Runs on the start caller's message-pump thread; must not retain message.
 */
typedef int (*av_window_notify_t)(const av_message_t *message, void *context);
/** @brief Start tracking visible application windows for one target process tree root.
 * @param[in] process_id Nonzero process ID to select; only this process is tracked.
 * @param[in] notify Nonnull borrowed synchronous callback retained until stop.
 * @param[in,out] context Optional caller-owned callback context.
 * @return 0 on success, -1 on invalid arguments/hook registration failure.
 * @note One active tracker per process; caller must pump Windows messages and
 * call av_windows_stop on the same thread. No heap allocation or ownership transfer.
 */
int av_windows_start(DWORD process_id, av_window_notify_t notify, void *context);
/** @brief Remove hooks and tracked handles after stopping the message pump.
 * @note Same thread as start; idempotent. No parameters/results/heap allocation.
 */
void av_windows_stop(void);
/** @brief Apply a host geometry/close request to a currently tracked window.
 * @param[in] message Nonnull borrowed validated control request.
 * @return 0 on success, -1 on stale handle/type/Win32 failure.
 * @note Tracker thread only; validates process ownership before using HWND.
 * Guest x/y remain unchanged; only size/fullscreen/minimize/WM_CLOSE supported.
 */
int av_windows_apply(const av_message_t *message);
#endif
