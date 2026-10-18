#ifndef WaddleLoaderEnvironmentWindowsH
#define WaddleLoaderEnvironmentWindowsH

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

/** Number of exact guarded loader keys; no unrelated key is mutated. */
#define WaddleLoaderEnvironmentVariableCount 4u
/** Per-key inline UTF-16 capacity including NUL; larger values fail before mutation. */
#define WaddleLoaderEnvironmentMaxValueUnits 32768u

/** @brief Read one UTF-16 process environment value.
 * @param[in] name Nonnull NUL-terminated key; borrowed for the call.
 * @param[out] value Nonnull buffer of capacity UTF-16 code units; caller-owned.
 * @param[in] capacity Buffer capacity, including room for the NUL terminator.
 * @return Value length excluding NUL, zero for an empty or absent value, or the
 * required length including NUL when capacity is insufficient. On zero, query
 * last_error: ERROR_ENVVAR_NOT_FOUND means absent; ERROR_SUCCESS means present-empty.
 * @note Must have Win32 GetEnvironmentVariableW semantics. Calls are serialized
 * with mutations of the four guarded keys by the caller.
 */
typedef DWORD (WINAPI *waddle_loader_environment_get_fn_t)(LPCWSTR name, LPWSTR value, DWORD capacity);

/** @brief Set or remove one UTF-16 process environment value.
 * @param[in] name Nonnull NUL-terminated key; borrowed for the call.
 * @param[in] value NUL-terminated replacement, or NULL to remove; borrowed.
 * @return Nonzero on success; zero on failure.
 * @note Must have Win32 SetEnvironmentVariableW semantics.
 */
typedef BOOL (WINAPI *waddle_loader_environment_set_fn_t)(LPCWSTR name, LPCWSTR value);

/** @brief Environment API hooks, copied into an active guard.
 * All callbacks are required and must have matching Win32 semantics.
 */
typedef struct waddle_loader_environment_api_t {
    /** GetVariable-compatible reader; NULL is not permitted. */
    waddle_loader_environment_get_fn_t get;
    /** SetVariable-compatible writer; NULL is not permitted. */
    waddle_loader_environment_set_fn_t set;
    /** Error reader paired with get; NULL is not permitted. */
    DWORD (WINAPI *get_last_error)(void);
    /** Error setter paired with get; NULL is not permitted. */
    void (WINAPI *set_last_error)(DWORD value);
} waddle_loader_environment_api_t;

/** @brief One desired key state for the guarded loader variables.
 * @param present Nonzero means set value; zero means remove the key.
 * @param value Borrowed NUL-terminated UTF-16 value when present; NULL is invalid.
 */
typedef struct waddle_loader_environment_override_t {
    /** Nonzero requests a present value; zero requests absence. */
    BOOL present;
    /** Borrowed UTF-16 string required when present; ignored when absent. */
    LPCWSTR value;
} waddle_loader_environment_override_t;

/** @brief Exact saved state for one guarded key; owned inline by the guard. */
typedef struct waddle_loader_environment_value_t {
    /** Saved length excluding the NUL terminator. */
    DWORD length;
    /** Distinguishes absent from present-empty. */
    BOOL present;
    /** Saved UTF-16 value including NUL when present, inline-owned. */
    WCHAR value[WaddleLoaderEnvironmentMaxValueUnits];
} waddle_loader_environment_value_t;

/** @brief Quiescent snapshot and retryable restoration owner.
 * @details Caller initializes with waddle_loader_environment_guard_init and
 * serializes access with process environment mutations. The value storage is
 * inline; no heap allocation or ownership transfer occurs. A failed restore
 * keeps this object active and its saved values intact for retry.
 */
typedef struct waddle_loader_environment_guard_t {
    /** Nonzero while snapshots must still be restored. */
    BOOL active;
    /** Bit i is set when restoration of key i failed on the latest attempt. */
    uint32_t restore_failed_mask;
    /** Copied callbacks used for capture, apply, and every restore retry. */
    waddle_loader_environment_api_t api;
    /** Inline snapshots in the documented four-key order. */
    waddle_loader_environment_value_t saved[WaddleLoaderEnvironmentVariableCount];
} waddle_loader_environment_guard_t;

/** @brief Status returned by environment guard operations. */
typedef enum waddle_loader_environment_status_t {
    /** Operation completed successfully. */
    WaddleLoaderEnvironmentOk = 0,
    /** Null argument, incomplete API table, or invalid override state. */
    WaddleLoaderEnvironmentInvalidArgument = 1,
    /** Begin was called while a previous snapshot still needs restoration. */
    WaddleLoaderEnvironmentAlreadyActive = 2,
    /** A value exceeded the fixed UTF-16 storage bound. */
    WaddleLoaderEnvironmentValueTooLong = 3,
    /** Capture failed before any process environment mutation. */
    WaddleLoaderEnvironmentCaptureFailed = 4,
    /** Apply failed, but rollback restored all four original states. */
    WaddleLoaderEnvironmentApplyFailed = 5,
    /** Apply failed and at least one rollback key still needs retry. */
    WaddleLoaderEnvironmentRollbackFailed = 6,
    /** A restore attempt failed; guard retains all snapshots for retry. */
    WaddleLoaderEnvironmentRestoreFailed = 7
} waddle_loader_environment_status_t;

/** @brief Initialize fresh or successfully restored guard storage.
 * @param[out] guard Nonnull caller-owned guard; no ownership transfer.
 * @return Nothing. Only call for a new or inactive guard.
 * @note Caller serializes calls and must not reinitialize an active guard.
 */
void waddle_loader_environment_guard_init(waddle_loader_environment_guard_t *guard);

/** @brief Return the Win32 environment API implementation.
 * @return Hooks for Get/SetEnvironmentVariableW and last-error access.
 * @note The returned record contains no owned resources.
 */
waddle_loader_environment_api_t waddle_loader_environment_windows_api(void);

/** @brief Capture every four loader keys, then apply requested values.
 * @param[in,out] guard Nonnull initialized inactive guard; retains snapshots if
 * application fails and rollback cannot restore every key.
 * @param[in] overrides Exactly four borrowed immutable states, in the documented key order;
 * record and strings must be disjoint from guard storage and accessible until return.
 * @param[in] api Nonnull complete borrowed API table; copied before use and may
 * alias guard.api. The overrides array and every present value must not overlap
 * guard storage because the guard is cleared after all overrides are validated.
 * @return Status: Ok; InvalidArgument; AlreadyActive; ValueTooLong; CaptureFailed
 * (with no process mutation); ApplyFailed (rollback completed); or RollbackFailed
 * (guard remains active and restore may be retried).
 * @note No allocation. Caller serializes environment access. Key order is
 * VK_DRIVER_FILES, VK_ICD_FILENAMES, VK_ADD_DRIVER_FILES, VK_LOADER_LAYERS_DISABLE.
 */
waddle_loader_environment_status_t waddle_loader_environment_begin(
    waddle_loader_environment_guard_t *guard,
    const waddle_loader_environment_override_t overrides[WaddleLoaderEnvironmentVariableCount],
    const waddle_loader_environment_api_t *api);

/** @brief Restore all four captured values, attempting every key on every call.
 * @param[in,out] guard Nonnull guard returned active by begin; retains saved
 * values when any SetEnvironmentVariableW call fails so caller can retry.
 * @return Ok when all four exact states are restored; RestoreFailed otherwise.
 * @note Idempotent after successful restore. Caller serializes environment access.
 */
waddle_loader_environment_status_t waddle_loader_environment_restore(
    waddle_loader_environment_guard_t *guard);

#endif
