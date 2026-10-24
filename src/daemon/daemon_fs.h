/**
 * @file daemon_fs.h
 * @brief VirtIO-FS (virtiofsd) lifecycle manager and filesystem path translation bridge.
 *
 * Provides virtiofsd binary discovery, argument vector building, process lifecycle
 * supervision, socket readiness polling, and integration with path translation rules.
 */

#ifndef WaddleDaemonFsH
#define WaddleDaemonFsH

#include "daemon_config.h"
#include "path_rules.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of command-line arguments for virtiofsd. */
#define VirtiofsMaxArgs UINT32_C(32)

/** @brief Default timeout in milliseconds waiting for virtiofsd socket creation. */
#define VirtiofsDefaultWaitMs UINT32_C(3000)

/**
 * @brief State structure for managed virtiofsd process instance.
 * @note Before spawn, caller initializes all fields to zero and sets log_fd to
 *       -1. A record with a PID, running flag, descriptor other than -1, or any
 *       nonempty path is already owned and cannot be reused.
 */
typedef struct virtiofs_process_t {
    /** @brief Process ID of spawned virtiofsd process (0 if not running). */
    pid_t pid;
    /** @brief UNIX socket path for vhost-user IPC. */
    char socket_path[WaddleMaxPathLen];
    /** @brief Log file path for stdout/stderr redirection. */
    char log_path[WaddleMaxPathLen];
    /** @brief Host directory path exported by this instance. */
    char shared_dir[WaddleMaxPathLen];
    /** @brief Open log file descriptor (-1 if closed). */
    int log_fd;
    /** @brief 1 if process is actively executing, 0 if terminated. */
    int is_running;
} virtiofs_process_t;

/**
 * @brief Locates the virtiofsd binary on the host system.
 *
 * Resolves the directory of the currently running executable via /proc/self/exe
 * and checks the bundled vendor directory (<exe_dir>/vendor/virtiofsd) before
 * falling back to standard system directories (/usr/libexec/virtiofsd,
 * /usr/lib/qemu/virtiofsd, etc.) and searching the PATH environment variable.
 *
 * @param[out] out_path Buffer of capacity at least WaddleMaxPathLen.
 * @param[in]  path_cap Capacity of out_path buffer in bytes.
 * @return 0 on success, or -1 if virtiofsd cannot be found.
 */
int daemon_fs_find_binary(char *out_path, size_t path_cap);

/**
 * @brief Constructs argument vector for virtiofsd.
 *
 * @param[in]  socket_path  Path to vhost-user UNIX socket.
 * @param[in]  shared_dir   Host directory to export.
 * @param[in]  sandbox_mode Sandbox mode (e.g. "none" or "auto").
 * @param[in]  cache_mode   Cache policy (e.g. "auto", "always", "none").
 * @param[out] argv         Array of char* pointers of capacity at least VirtiofsMaxArgs.
 * @param[in]  max_args     Capacity of argv array.
 * @return Number of arguments populated, or -1 on error.
 */
int daemon_fs_build_args(const char *socket_path,
                         const char *shared_dir,
                         const char *sandbox_mode,
                         const char *cache_mode,
                         char **argv,
                         size_t max_args);

/**
 * @brief Build an owned virtiofsd argv with an explicit read-only policy.
 * @param[in] socket_path Non-NULL borrowed absolute NUL-terminated path shorter
 *                        than WaddleMaxPathLen; retained only as an argv copy.
 * @param[in] shared_dir Non-NULL borrowed absolute path with the same bound.
 * @param[in] sandbox_mode Nullable borrowed mode shorter than48 bytes;
 *                         NULL/empty selects none.
 * @param[in] cache_mode Nullable borrowed mode shorter than48 bytes;
 *                       NULL/empty selects auto.
 * @param[in] read_only Exactly0 or1;1 appends --readonly. Unsupported binaries
 *                      fail at execution; no silent read-write fallback exists.
 * @param[out] argv Non-NULL disjoint caller-owned array of max_args pointers.
 * @param[in] max_args Actual pointer capacity, at least8, including trailing NULL.
 * @return Count excluding NULL, or -1/EINVAL, E2BIG or ENOMEM. Every error
 *         frees partial strings and sets argv[0] NULL when capacity permits.
 * @note Caller releases strings with daemon_fs_free_args. No input is retained;
 *       independent calls are thread-safe. The legacy builder selects read-write.
 */
int daemon_fs_build_args_for_mount(const char *socket_path, const char *shared_dir,
                                   const char *sandbox_mode, const char *cache_mode,
                                   uint32_t read_only, char **argv, size_t max_args);

/**
 * @brief Releases argument vector allocated by daemon_fs_build_args.
 *
 * @param[in,out] argv Array of argument strings.
 * @param[in]     argc Number of valid argument strings.
 */
void daemon_fs_free_args(char **argv, size_t argc);

/**
 * @brief Spawns virtiofsd and waits for its vhost-user UNIX socket to become ready.
 *
 * @param[out] proc         Non-null pointer to virtiofs_process_t to initialize.
 * @param[in]  socket_path  Destination UNIX socket path.
 * @param[in]  shared_dir   Host directory to export.
 * @param[in]  log_path     Log file path for stdout/stderr.
 * @param[in]  binary_path  Path to virtiofsd binary (NULL to auto-detect).
 * @param[in]  timeout_ms   Maximum time to wait for socket availability.
 * @return 0 on success, or -1 on failure.
 */
int daemon_fs_spawn(virtiofs_process_t *proc,
                    const char *socket_path,
                    const char *shared_dir,
                    const char *log_path,
                    const char *binary_path,
                    uint32_t timeout_ms);

/**
 * @brief Start one owned backend with the configured read-only policy.
 * @param[out] proc Caller-owned unused process record with pid 0, running false,
 *                  empty paths, and log_fd -1; owned records are rejected EBUSY.
 * @param[in] socket_path Non-NULL borrowed absolute bounded socket path.
 * @param[in] shared_dir Non-NULL borrowed absolute bounded directory path.
 * @param[in] read_only Exactly0 or1;1 requires backend --readonly support.
 * @param[in] log_path Non-NULL borrowed absolute path shorter thanWaddleMaxPathLen.
 * @param[in] binary_path Nullable borrowed executable path; NULL selects lookup.
 * @param[in] timeout_ms Readiness deadline;0 selects VirtiofsDefaultWaitMs.
 * @return 0 on ready, -1 with errno on argument, process or readiness failure.
 * @note Caller owns proc and must call daemon_fs_cleanup after use, including
 *       failed calls that retain a child when teardown fails. No input is
 *       retained. External serialization is required for a shared proc/path;
 *       calls with independent records/paths are thread-safe. The legacy spawn
 *       selects read-write. No successful spawn implies guest-drive acceptance.
 */
int daemon_fs_spawn_for_mount(virtiofs_process_t *proc, const char *socket_path,
                              const char *shared_dir, uint32_t read_only,
                              const char *log_path, const char *binary_path,
                              uint32_t timeout_ms);

/**
 * @brief Checks if virtiofsd is running and reaps child if terminated.
 *
 * @param[in,out] proc virtiofs process handle.
 * @return 1 if running, 0 if terminated, or -1 on error.
 */
int daemon_fs_poll_status(virtiofs_process_t *proc);

/**
 * @brief Gracefully terminates virtiofsd via SIGTERM and reaps it.
 *
 * @param[in,out] proc       virtiofs process handle.
 * @param[in]     timeout_ms Timeout before SIGKILL fallback.
 * @return 0 only after child reaping or ECHILD confirmation and socket cleanup;
 *         -1 on kill/wait/unlink error. EINTR is retried. Kill errors other than
 *         ESRCH and wait errors other than ECHILD preserve the owned PID and all
 *         associated paths/descriptors so the caller can retry.
 */
int daemon_fs_stop(virtiofs_process_t *proc, uint32_t timeout_ms);

/**
 * @brief Converts daemon mount descriptors to path_rule_t rules for path translation.
 *
 * @param[in]  mounts      Non-NULL borrowed mount_count descriptors, retained by caller.
 * @param[in]  mount_count Number of mounts in array.
 * @param[out] rules       Non-NULL caller-owned destination; unchanged on failure.
 * @param[in]  max_rules   Capacity of rules array.
 * @return Number of rules populated, or -1 with EINVAL for invalid/unterminated
 * mounts or count above WaddleMaxMounts; E2BIG for insufficient output capacity.
 * @note No allocation; output borrows strings until mounts are changed/freed.
 * Thread-safe for independent output and immutable input. Never truncates mounts.
 */
int daemon_fs_mounts_to_rules(const waddle_daemon_fs_mount_t *mounts,
                              size_t mount_count,
                              path_rule_t *rules,
                              size_t max_rules);

/**
 * @brief Translates a host POSIX path to a guest Windows path using active daemon config.
 *
 * @param[in] path Non-NULL borrowed NUL-terminated path. Guest-native/relative
 * strings pass through; absolute POSIX paths must match a configured export.
 * @param[in] config Non-NULL borrowed immutable config; count bounded by WaddleMaxMounts.
 * @return Owned malloc string, caller frees via free(), or NULL with EINVAL for
 * invalid/unmapped input, E2BIG for oversized paths or ENOMEM for allocation failure.
 * @note No implicit root export. Longest component-prefix match wins; explicit
 * root exports are supported. Does not canonicalize symlinks or authorize access.
 * No retained pointers; thread-safe for immutable config and independent calls.
 */
char *daemon_fs_translate_path(const char *path, const daemon_config_t *config);

/**
 * @brief Cleans up virtiofsd process, socket file, and descriptors.
 *
 * @param[in,out] proc Process handle; it must remain alive through retries.
 * @note If stop/reap or owned-socket removal fails, returns without closing the
 *       log descriptor, unlinking further paths, or clearing ownership fields.
 *       After success it closes the log descriptor, clears every path/PID field,
 *       sets log_fd to -1, and is idempotent.
 */
void daemon_fs_cleanup(virtiofs_process_t *proc);

#ifdef __cplusplus
}
#endif

#endif /* WaddleDaemonFsH */
