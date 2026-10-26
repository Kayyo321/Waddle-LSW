/**
 * @file guest_launch.h
 * @brief Guest launch executor: LaunchRequest in, LaunchResult out, Win32 behind a native table.
 *
 * Contract: impl/seamless-interop/claude_45_percent/guest_launch_contract.md and IMPL_DESC 3.9.3, 6.5.
 * The module performs no I/O of its own. Every Win32 side effect goes through guest_launch_native_t,
 * so the portable core is tested on Linux with a double and the real table lives in guest_launch_windows.c.
 *
 * Threading: guest_launch_execute keeps no state between calls. It is reentrant when the native table
 * and the resolver are. The caller runs it on a launch worker, never on the control thread that
 * services the wire; the call returns after at most one native launch request and never waits for
 * the launched process to exit.
 */
#ifndef WADDLE_GUEST_LAUNCH_H
#define WADDLE_GUEST_LAUNCH_H

#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_protocol.h"

/** @brief Largest guest path, AUMID, working directory or message text, in UTF-8 bytes (excluding NUL). */
#define GuestLaunchMaxText 1024u
/** @brief Largest arguments string in UTF-8 bytes (excluding NUL). */
#define GuestLaunchMaxArguments 4096u
/** @brief Largest command line built by the executor, in UTF-8 bytes. Below CreateProcessW's 32767-unit limit. */
#define GuestLaunchMaxCommandLine 8192u
/** @brief Output capacity required by guest_launch_execute: enough for a header and every LaunchResult field. */
#define GuestLaunchResultBytes 2048u

/**
 * @brief Result of one native call.
 * @note `error` is a Win32 error code (0 = success). `process` is an owned process HANDLE or NULL; when
 *       non-NULL the executor closes it exactly once, on every path, through close_process.
 */
typedef struct guest_launch_native_result_t {
    uint32_t error; /**< 0 success; otherwise a Win32 error code (see guest_launch_status_for_error). */
    uint32_t process_id; /**< Process id of the launched process, or 0 when none is known. */
    void *process; /**< Owned process handle or NULL. Must be NULL when error is nonzero. */
} guest_launch_native_result_t;

/**
 * @brief ShellExecuteExW parameters after UTF-8 to UTF-16 conversion by the native table.
 * @note NULL members are passed to Win32 as NULL. `verb` is always "open" for this module.
 */
typedef struct guest_launch_shell_t {
    const char *verb; /**< Verb, "open". */
    const char *file; /**< File, shell item, or program name. Never NULL. */
    const char *parameters; /**< Parameter string or NULL. */
    const char *directory; /**< lpDirectory or NULL. */
} guest_launch_shell_t;

/**
 * @brief Win32 side effects of the executor. Every function fills `result` and returns nothing.
 * @note The table is caller-owned and must outlive every call. Functions run on the launch worker.
 */
typedef struct guest_launch_native_t {
    void *context; /**< Passed as the first argument of every function. May be NULL. */
    /** @brief ShellExecuteExW with SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI, nShow SW_SHOWNORMAL. */
    void (*shell_execute)(void *context, const guest_launch_shell_t *shell, guest_launch_native_result_t *result);
    /** @brief CreateProcessW with CREATE_NEW_CONSOLE. `command_line` is UTF-8; `directory` may be NULL. */
    void (*create_console)(void *context, const char *command_line, const char *directory,
                           guest_launch_native_result_t *result);
    /** @brief AUMID activation seam for packaged apps. `arguments` is UTF-8 and may be empty. */
    void (*activate_aumid)(void *context, const char *aumid, const char *arguments,
                           guest_launch_native_result_t *result);
    /** @brief CloseHandle on a process handle previously returned in a result. Called exactly once per handle. */
    void (*close_process)(void *context, void *process);
} guest_launch_native_t;

/**
 * @brief Launch target of one catalog entry, as resolved for an App request. NUL-terminated copies.
 * @note Filled by the resolver. The executor validates every string again with the wire rules.
 */
typedef struct guest_launch_target_t {
    uint32_t packaged; /**< 1 when the catalog marks the AUMID as a packaged (MSIX) app. */
    uint32_t aumid_length; /**< AUMID bytes, 0 when none. */
    char aumid[GuestLaunchMaxText + 1u]; /**< NUL-terminated AUMID. */
    uint32_t path_length; /**< Target path bytes, 0 when none. */
    char path[GuestLaunchMaxText + 1u]; /**< NUL-terminated guest path. */
    uint32_t arguments_length; /**< Arguments bytes, 0 when none. */
    char arguments[GuestLaunchMaxArguments + 1u]; /**< NUL-terminated arguments. */
    uint32_t working_dir_length; /**< Working directory bytes, 0 when none. */
    char working_dir[GuestLaunchMaxText + 1u]; /**< NUL-terminated working directory. */
} guest_launch_target_t;

/**
 * @brief Catalog lookup for App requests. Owner-thread or launch-worker safe as the catalog runtime requires.
 * @param[in] context Caller context.
 * @param[in] app_key Borrowed app key bytes, not NUL-terminated.
 * @param[in] app_key_length Bytes in app_key.
 * @param[out] target Caller-owned output, written only when the return value is 0.
 * @return 0 resolved; 1 no such app key; -1 catalog failure.
 */
typedef int (*guest_launch_resolve_fn)(void *context, const uint8_t *app_key, size_t app_key_length,
                                       guest_launch_target_t *target);

/** @brief Everything the executor needs from its caller. Borrowed; must outlive each call. */
typedef struct guest_launch_env_t {
    const guest_launch_native_t *native; /**< Required native table. */
    guest_launch_resolve_fn resolve; /**< Required for App requests; may be NULL when no App is ever sent. */
    void *resolve_context; /**< Passed to resolve. */
} guest_launch_env_t;

/**
 * @brief Map a Win32 error code to a LaunchResult status (IMPL_DESC 3.2.4 `interop_status_t`).
 * @param[in] error Win32 error code; 0 means success.
 * @return InteropOk for 0; InteropNotFound, InteropAccessDenied, InteropCancelled, InteropTimedOut,
 *         InteropBusy, InteropUnsupported for the mapped codes; InteropGuestError for everything else.
 * @note Pure and thread-safe. The mapping table is in guest_launch.c.
 */
interop_status_t guest_launch_status_for_error(uint32_t error);

/**
 * @brief Execute one LaunchRequest frame and encode its LaunchResult frame.
 *
 * @param[in] env Nonnull; env->native and its function pointers must be set.
 * @param[in] request Nonnull borrowed bytes of one complete LaunchRequest frame (header and payload).
 * @param[in] request_length Exact frame length: InteropHeaderBytes plus the header payload length.
 * @param[out] result Nonnull writable buffer of at least GuestLaunchResultBytes bytes.
 * @param[in] result_capacity Capacity of result. Checked before any native call.
 * @param[out] result_length Nonnull; receives the LaunchResult frame length on success.
 * @return 0 when a LaunchResult frame was written, including every launch failure (the status says which);
 *         -1 when no reply can be formed: NULL argument, capacity below GuestLaunchResultBytes, a header that
 *         does not decode, a frame whose length disagrees with its header, or a type other than LaunchRequest.
 *         On -1 no native call was made and the caller sends nothing.
 * @note Exactly one native call is made for a request that reaches the launch stage. Every process handle
 *       the native table returns is closed exactly once before this function returns.
 * @thread_safety Reentrant when the native table and resolver are reentrant.
 */
int guest_launch_execute(const guest_launch_env_t *env, const uint8_t *request, size_t request_length,
                         uint8_t *result, size_t result_capacity, size_t *result_length);

#endif
